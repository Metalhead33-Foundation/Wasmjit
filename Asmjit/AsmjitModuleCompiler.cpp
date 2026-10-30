#include "AsmjitModuleCompiler.hpp"

#include "../WasmBase/WasmOpcodeDispatcher.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmTrap.hpp"
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
			const WASM::FuncType& type, const WASM::FunctionBody& body)
		: compiler(compiler)
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
		asmjit::x86::Gp vmContext = compiler.new_gp_ptr("vmContext");
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
	enum class BinaryOp { Add, Sub, Mul };

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
		}
		values.push_back(lhs);
	}

	asmjit::x86::Compiler& compiler;
	std::vector<asmjit::x86::Gp> locals;
	std::vector<asmjit::x86::Gp> values;
	uint32_t resultCount = 0;
};

} // namespace

struct ModuleCompiler::FunctionRecord {
	const asmjit::FuncSignature* signature = nullptr;
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
		for (uint32_t i = 0; i < module.functionBodies.size(); ++i) {
			const uint32_t typeIndex = module.internalFunctionTypeIndices[i];
			const WASM::FuncType type = module.funcTypeAt(typeIndex);
			if (type.results.size() > 1)
				throw std::runtime_error("AsmJit initial compiler does not support multi-value results");

			asmjit::CodeHolder code;
			checkError(code.init(context.environment(), context.cpuFeatures()),
					"AsmJit CodeHolder initialisation failed");
			asmjit::x86::Compiler compiler(&code);
			auto* function = compiler.add_func(*functionRecords[i]->signature);
			if (function == nullptr)
				throw std::runtime_error("AsmJit failed to declare a function body");

			I32FunctionEmitter emitter(compiler, *function, type, module.functionBodies[i]);
			emitter.compile(module.functionBodies[i], type);
			checkError(compiler.end_func(), "AsmJit function finalisation failed");
			checkError(compiler.finalize(), "AsmJit compiler finalisation failed");

			functionRecords[i]->entry = context.install(code);
			internals.internalCallables[i].rawFnPtr = functionRecords[i]->entry;
			internals.internalCallables[i].fnPtr = functionRecords[i]->entry;
		}
	} catch (...) {
		// Instantiation is transactional with respect to executable memory:
		// failed modules must not pin earlier function bodies in the session.
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
