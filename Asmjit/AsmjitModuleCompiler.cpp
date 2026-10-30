#include "AsmjitModuleCompiler.hpp"

#include "../WasmBase/WasmOpcodeDispatcher.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmVMContext.hpp"
#include "../WasmBase/WasmType.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <cstddef>
#include <cstdint>
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

void requireI32(const WASM::ValueType& type, const char* role)
{
	if (type.opcode != WASM::ValueTypeCode::I32)
		throw std::runtime_error(std::string("AsmJit initial compiler supports only i32 ")
				+ role);
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

void* asmjitMemoryAddress(WASM::VMContext* vm, uint32_t memoryIndex,
		uint32_t address, uint32_t offset, uint32_t width) noexcept
{
	if (vm == nullptr || memoryIndex >= vm->memoryCount) {
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
	return memory->memoryBase + effective;
}

asmjit::FuncSignature stackCheckSignature()
{
	asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
	signature.set_ret(asmjit::TypeId::kVoid);
	signature.add_arg(asmjit::TypeId::kUIntPtr);
	return signature;
}

class I32FunctionEmitter final : public WASM::OpcodeDispatcher
{
public:
	I32FunctionEmitter(asmjit::x86::Compiler& compiler, asmjit::FuncNode& function,
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
			requireI32(parameter.val, "parameters");
		for (const auto& result : type.results)
			requireI32(result.val, "results");
		for (const auto& local : stack.allLocals)
			requireI32(local, "locals");

		locals.reserve(stack.allLocals.size());
		vmContext = compiler.new_gp_ptr("vmContext");
		function.set_arg(0, vmContext);

		const asmjit::FuncSignature checkSignature = stackCheckSignature();
		asmjit::InvokeNode* checkCall = nullptr;
		checkError(compiler.invoke(&checkCall,
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
		checkError(compiler.invoke(&trapSlotCall,
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
		} else {
			asmjit::x86::Gp zero = compiler.new_gp32("trapResult");
			checkError(compiler.xor_(zero, zero), "AsmJit trap-result zeroing failed");
			checkError(compiler.ret(zero), "AsmJit trap-path result return emission failed");
		}
		checkError(compiler.bind(bodyLabel), "AsmJit body label binding failed");

		for (uint32_t i = 0; i < stack.parameterCount; ++i) {
			asmjit::x86::Gp parameter = compiler.new_gp32("parameter");
			function.set_arg(i + 1, parameter);
			locals.push_back(parameter);
		}
		for (uint32_t i = 0; i < stack.localCount; ++i) {
			asmjit::x86::Gp local = compiler.new_gp32("local");
			checkError(compiler.xor_(local, local), "AsmJit local zero-initialisation failed");
			locals.push_back(local);
		}
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
				throw std::runtime_error("AsmJit i32 function must end with exactly one result");
			checkError(compiler.ret(values.back()), "AsmJit i32 function return emission failed");
		}
		(void)type;
	}

protected:
	void dispatchEnd() override {}

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
		asmjit::x86::Gp copy = compiler.new_gp32("localCopy");
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
		checkError(compiler.mov(locals[i], value), "AsmJit local.set emission failed");
	}

	void dispatchLocalTee(WASM::LocalIdx index) override
	{
		const size_t i = static_cast<size_t>(index);
		if (i >= locals.size() || values.empty())
			throw std::runtime_error("AsmJit local.tee stack/index underflow");
		asmjit::x86::Gp copy = compiler.new_gp32("teeCopy");
		checkError(compiler.mov(copy, values.back()), "AsmJit local.tee emission failed");
		checkError(compiler.mov(locals[i], copy), "AsmJit local.tee store emission failed");
		values.back() = copy;
	}

	void dispatchDrop() override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit drop operand stack underflow");
		values.pop_back();
	}

	void dispatchBlock(const WASM::BlockType& type) override
	{
		if (!type.isVoid())
			throw std::runtime_error("AsmJit structured control flow currently requires void block types");
		ControlFrame frame;
		frame.kind = ControlKind::Block;
		frame.stackHeight = values.size();
		frame.end = compiler.new_label();
		controls.push_back(frame);
	}

	void dispatchLoop(const WASM::BlockType& type) override
	{
		if (!type.isVoid())
			throw std::runtime_error("AsmJit structured control flow currently requires void loop types");
		ControlFrame frame;
		frame.kind = ControlKind::Loop;
		frame.stackHeight = values.size();
		frame.begin = compiler.new_label();
		frame.end = compiler.new_label();
		checkError(compiler.bind(frame.begin), "AsmJit loop label binding failed");
		controls.push_back(frame);
	}

	void dispatchIf(const WASM::BlockType& type) override
	{
		if (!type.isVoid())
			throw std::runtime_error("AsmJit structured control flow currently requires void if types");
		if (values.empty())
			throw std::runtime_error("AsmJit if condition stack underflow");
		auto condition = values.back();
		values.pop_back();
		ControlFrame frame;
		frame.kind = ControlKind::If;
		frame.stackHeight = values.size();
		frame.alternate = compiler.new_label();
		frame.end = compiler.new_label();
		checkError(compiler.test(condition, condition), "AsmJit if condition test failed");
		checkError(compiler.jz(frame.alternate), "AsmJit if condition branch failed");
		controls.push_back(frame);
	}

	void dispatchElse() override
	{
		if (controls.empty() || controls.back().kind != ControlKind::If || controls.back().hasElse)
			throw std::runtime_error("AsmJit encountered an unmatched or duplicate else");
		ControlFrame& frame = controls.back();
		if (values.size() != frame.stackHeight)
			throw std::runtime_error("AsmJit if arm leaves an unexpected operand stack shape");
		checkError(compiler.jmp(frame.end), "AsmJit if/else end jump failed");
		checkError(compiler.bind(frame.alternate), "AsmJit else label binding failed");
		frame.hasElse = true;
	}

	void dispatchEnd() override
	{
		// The outermost end closes the function body, not a nested control frame.
		if (controls.empty())
			return;
		ControlFrame frame = controls.back();
		controls.pop_back();
		if (values.size() != frame.stackHeight)
			throw std::runtime_error("AsmJit structured block leaves an unexpected operand stack shape");
		if (frame.kind == ControlKind::If && !frame.hasElse)
			checkError(compiler.bind(frame.alternate), "AsmJit implicit else label binding failed");
		checkError(compiler.bind(frame.end), "AsmJit structured block end binding failed");
	}

	void dispatchBrIf(WASM::LabelIdx depth) override
	{
		if (values.empty())
			throw std::runtime_error("AsmJit br_if condition stack underflow");
		if (depth >= controls.size())
			throw std::runtime_error("AsmJit br_if label depth is out of range");
		const auto condition = values.back();
		values.pop_back();
		const ControlFrame& target = controls[controls.size() - 1 - depth];
		if (values.size() != target.stackHeight)
			throw std::runtime_error("AsmJit br_if currently supports only void label signatures");
		const asmjit::Label label = target.kind == ControlKind::Loop ? target.begin : target.end;
		checkError(compiler.test(condition, condition), "AsmJit br_if condition test failed");
		checkError(compiler.jnz(label), "AsmJit br_if branch emission failed");
	}

	void dispatchBr(WASM::LabelIdx depth) override
	{
		if (depth >= controls.size())
			throw std::runtime_error("AsmJit br label depth is out of range");
		const ControlFrame& target = controls[controls.size() - 1 - depth];
		if (values.size() != target.stackHeight)
			throw std::runtime_error("AsmJit br currently supports only void label signatures");
		const asmjit::Label label = target.kind == ControlKind::Loop ? target.begin : target.end;
		checkError(compiler.jmp(label), "AsmJit br emission failed");
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
			requireI32(parameter.val, "call parameters");
		for (const auto& result : calledType.results)
			requireI32(result.val, "call results");
		if (values.size() < calledType.params.size())
			throw std::runtime_error("AsmJit call operand stack underflow");

		std::vector<asmjit::x86::Gp> arguments(calledType.params.size());
		for (size_t i = calledType.params.size(); i > 0; --i) {
			arguments[i - 1] = values.back();
			values.pop_back();
		}

		asmjit::InvokeNode* invoke = nullptr;
		const asmjit::Error error = isInternal
				? compiler.invoke(&invoke, targetLabel, *signatures[typeIndex])
				: compiler.invoke(&invoke, target, *signatures[typeIndex]);
		checkError(error, "AsmJit function-call emission failed");
		if (isInternal) {
			invoke->set_arg(0, vmContext);
		} else {
			// Wasm-to-Wasm imports execute in the exporting instance's context;
			// native imports (null context) execute against the caller's context.
			WASM::VMContext* calleeContext = internals.importStorage[index].context;
			if (calleeContext == nullptr) {
				invoke->set_arg(0, vmContext);
			} else {
				auto importedContext = compiler.new_gp_ptr("importedVmContext");
				checkError(compiler.mov(importedContext,
						reinterpret_cast<uint64_t>(calleeContext)),
						"AsmJit imported VMContext materialisation failed");
				invoke->set_arg(0, importedContext);
			}
		}
		for (size_t i = 0; i < arguments.size(); ++i)
			invoke->set_arg(i + 1, arguments[i]);
		if (!calledType.results.empty()) {
			auto result = compiler.new_gp32("callResult");
			invoke->set_ret(0, result);
			values.push_back(result);
		}
		emitPendingTrapReturn();
	}

	void emitPendingTrapReturn()
	{
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		asmjit::InvokeNode* slotCall = nullptr;
		checkError(compiler.invoke(&slotCall,
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
		} else {
			auto zero = compiler.new_gp32("postCallTrapResult");
			checkError(compiler.xor_(zero, zero), "AsmJit post-call trap result initialisation failed");
			checkError(compiler.ret(zero), "AsmJit post-call trap result return failed");
		}
		checkError(compiler.bind(noTrap), "AsmJit post-call continuation binding failed");
	}

	void dispatchI32Load(WASM::MemArg arg) override { emitMemoryLoad(arg, 4, LoadKind::I32); }
	void dispatchI32Load8S(WASM::MemArg arg) override { emitMemoryLoad(arg, 1, LoadKind::I8Signed); }
	void dispatchI32Load8U(WASM::MemArg arg) override { emitMemoryLoad(arg, 1, LoadKind::I8Unsigned); }
	void dispatchI32Load16S(WASM::MemArg arg) override { emitMemoryLoad(arg, 2, LoadKind::I16Signed); }
	void dispatchI32Load16U(WASM::MemArg arg) override { emitMemoryLoad(arg, 2, LoadKind::I16Unsigned); }
	void dispatchI32Store(WASM::MemArg arg) override { emitMemoryStore(arg, 4); }
	void dispatchI32Store8(WASM::MemArg arg) override { emitMemoryStore(arg, 1); }
	void dispatchI32Store16(WASM::MemArg arg) override { emitMemoryStore(arg, 2); }

	void dispatchNop() override {}

	void dispatchI32Extend8S() override { emitSignExtend(8); }
	void dispatchI32Extend16S() override { emitSignExtend(16); }

	void dispatchI32Clz() override { emitBitCount(BitCountOp::LeadingZeros); }
	void dispatchI32Ctz() override { emitBitCount(BitCountOp::TrailingZeros); }
	void dispatchI32Popcnt() override { emitUnaryCall(&asmjitI32Popcnt, "i32.popcnt"); }
	void dispatchI32DivS() override { emitCheckedBinaryCall(&asmjitI32DivS, "i32.div_s"); }
	void dispatchI32DivU() override { emitCheckedBinaryCall(&asmjitI32DivU, "i32.div_u"); }
	void dispatchI32RemS() override { emitCheckedBinaryCall(&asmjitI32RemS, "i32.rem_s"); }
	void dispatchI32RemU() override { emitCheckedBinaryCall(&asmjitI32RemU, "i32.rem_u"); }

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

	void dispatchReturn() override
	{
		if (resultCount == 0) {
			checkError(compiler.ret(), "AsmJit return emission failed");
			return;
		}
		if (values.empty())
			throw std::runtime_error("AsmJit return operand stack underflow");
		checkError(compiler.ret(values.back()), "AsmJit return emission failed");
	}

private:
	enum class BitCountOp { LeadingZeros, TrailingZeros };
	enum class LoadKind { I32, I8Signed, I8Unsigned, I16Signed, I16Unsigned };

	enum class ControlKind { Block, Loop, If };
	struct ControlFrame {
		ControlKind kind = ControlKind::Block;
		size_t stackHeight = 0;
		asmjit::Label begin;
		asmjit::Label alternate;
		asmjit::Label end;
		bool hasElse = false;
	};

	enum class BinaryOp { Add, Sub, Mul, And, Or, Xor };
	enum class ShiftOp { Left, ArithmeticRight, LogicalRight, RotateLeft, RotateRight };

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

	void validateMemoryArg(const WASM::MemArg& arg, uint32_t width) const
	{
		if (arg.isArray)
			throw std::runtime_error("AsmJit scalar loads/stores do not support GC-array memargs");
		if (arg.memidx >= module.importMemories.size() + module.memories.size())
			throw std::runtime_error("AsmJit memory instruction references an unknown memory");
		const WASM::MemoryType& type = arg.memidx < module.importMemories.size()
				? module.importMemories[arg.memidx].memory
				: module.memories[arg.memidx - module.importMemories.size()];
		if (type.indexType != WASM::MemoryIndexType::I32)
			throw std::runtime_error("AsmJit initial memory lowering supports memory32 only");
		if (arg.offset > UINT32_MAX)
			throw std::runtime_error("AsmJit memory32 offset exceeds the encoded u32 range");
		(void)width;
	}

	asmjit::x86::Gp emitMemoryAddress(const WASM::MemArg& arg, uint32_t width,
			const asmjit::x86::Gp& wasmAddress)
	{
		validateMemoryArg(arg, width);
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(&invoke,
				reinterpret_cast<uint64_t>(&asmjitMemoryAddress), signature),
				"AsmJit memory-address helper call emission failed");
		invoke->set_arg(0, vmContext);
		auto memoryIndex = compiler.new_gp32("memoryIndex");
		checkError(compiler.mov(memoryIndex, static_cast<uint32_t>(arg.memidx)),
				"AsmJit memory-index materialisation failed");
		invoke->set_arg(1, memoryIndex);
		invoke->set_arg(2, wasmAddress);
		auto offset = compiler.new_gp32("memoryOffset");
		checkError(compiler.mov(offset, static_cast<uint32_t>(arg.offset)),
				"AsmJit memory-offset materialisation failed");
		invoke->set_arg(3, offset);
		auto accessWidth = compiler.new_gp32("memoryWidth");
		checkError(compiler.mov(accessWidth, width), "AsmJit memory-width materialisation failed");
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
		auto result = compiler.new_gp32("memoryLoad");
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
			checkError(compiler.mov(asmjit::x86::byte_ptr(pointer), value.r8()), "AsmJit i32.store8 emission failed"); break;
		case 2:
			checkError(compiler.mov(asmjit::x86::word_ptr(pointer), value.r16()), "AsmJit i32.store16 emission failed"); break;
		case 4:
			checkError(compiler.mov(asmjit::x86::dword_ptr(pointer), value), "AsmJit i32.store emission failed"); break;
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

	template <typename Function>
	void emitUnaryCall(uint32_t (*function)(uint32_t), const char* operation)
	{
		if (values.empty())
			throw std::runtime_error(std::string("AsmJit ") + operation + " operand stack underflow");
		auto argument = values.back(); values.pop_back();
		asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
		signature.set_ret(asmjit::TypeId::kUInt32);
		signature.add_arg(asmjit::TypeId::kUInt32);
		asmjit::InvokeNode* invoke = nullptr;
		checkError(compiler.invoke(&invoke, reinterpret_cast<uint64_t>(function), signature),
				"AsmJit unary arithmetic helper call emission failed");
		invoke->set_arg(0, argument);
		auto result = compiler.new_gp32("unaryArithmeticResult");
		invoke->set_ret(0, result);
		values.push_back(result);
	}

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
		checkError(compiler.invoke(&invoke, reinterpret_cast<uint64_t>(function), signature),
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

	void emitCompare(asmjit::x86::CondCode condition)
	{
		if (values.size() < 2)
			throw std::runtime_error("AsmJit i32 comparison operand stack underflow");
		auto rhs = values.back(); values.pop_back();
		auto lhs = values.back(); values.pop_back();
		auto result = compiler.new_gp32("comparison");
		checkError(compiler.xor_(result, result), "AsmJit comparison result initialisation failed");
		checkError(compiler.cmp(lhs, rhs), "AsmJit comparison emission failed");
		checkError(compiler.set(condition, result.r8()), "AsmJit comparison condition emission failed");
		values.push_back(result);
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
	std::vector<asmjit::x86::Gp> values;
	std::vector<ControlFrame> controls;
	uint32_t resultCount = 0;
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
		internals.compiledFunctions[i] = record.get();
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
			I32FunctionEmitter emitter(compiler, *function, type, module.functionBodies[i],
					module, internals, functions, signatures);
			emitter.compile(module.functionBodies[i], type);
			checkError(compiler.end_func(), "AsmJit function finalisation failed");
		}

		checkError(compiler.finalize(), "AsmJit compiler finalisation failed");
		void* codeBase = context.install(code);
		for (uint32_t i = 0; i < functionRecords.size(); ++i) {
			const uint64_t offset = code.label_offset(functions[i]->label());
			functionRecords[i]->entry = static_cast<uint8_t*>(codeBase) + offset;
			internals.internalCallables[i].rawFnPtr = functionRecords[i]->entry;
			internals.internalCallables[i].fnPtr = functionRecords[i]->entry;
		}
	} catch (...) {
		// Instantiation is transactional with respect to executable memory:
		// a failed module must not pin partially generated code in the session.
		context.releaseAllocationsSince(allocationCheckpoint);
		for (uint32_t i = 0; i < functionRecords.size(); ++i) {
			functionRecords[i]->entry = nullptr;
			internals.internalCallables[i].rawFnPtr = nullptr;
			internals.internalCallables[i].fnPtr = nullptr;
		}
		throw;
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
