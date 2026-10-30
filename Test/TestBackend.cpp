#include "TestBackend.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <jit/jit.h>

#include "Asmjit/AsmjitContext.hpp"
#include "Asmjit/AsmjitModuleCompiler.hpp"
#include "LibJit/LibJitContext.hpp"
#include "LibJit/LibjitModuleCompiler.hpp"
#include "WasmBase/WasmDiagnostics.hpp"

namespace TestBackend {

namespace {

// The host C ABI's shape for a `v128` argument: 16 bytes, no padding, which
// System V classifies as two INTEGER eightbytes.
struct AbiV128 { uint32_t lane[4]; };

template <typename Ret>
Ret callWithV128Params(void* fnPtr, void* context, const AbiV128* args, size_t count)
{
	switch (count) {
	case 1:
		return reinterpret_cast<Ret (*)(void*, AbiV128)>(fnPtr)(context, args[0]);
	case 2:
		return reinterpret_cast<Ret (*)(void*, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1]);
	case 3:
		return reinterpret_cast<Ret (*)(void*, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2]);
	default:
		return reinterpret_cast<Ret (*)(void*, AbiV128, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2], args[3]);
	}
}

void callWithV128ParamsVoid(void* fnPtr, void* context, const AbiV128* args, size_t count)
{
	switch (count) {
	case 1:
		reinterpret_cast<void (*)(void*, AbiV128)>(fnPtr)(context, args[0]);
		return;
	case 2:
		reinterpret_cast<void (*)(void*, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1]);
		return;
	case 3:
		reinterpret_cast<void (*)(void*, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2]);
		return;
	default:
		reinterpret_cast<void (*)(void*, AbiV128, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2], args[3]);
		return;
	}
}

template <typename Ret>
Ret callI32Function(void* function, void* vm, const std::vector<const void*>& args)
{
	auto value = [&](std::size_t i) { return *static_cast<const std::int32_t*>(args[i]); };
	switch (args.size()) {
	case 0: return reinterpret_cast<Ret (*)(void*)>(function)(vm);
	case 1: return reinterpret_cast<Ret (*)(void*, std::int32_t)>(function)(vm, value(0));
	case 2: return reinterpret_cast<Ret (*)(void*, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1));
	case 3: return reinterpret_cast<Ret (*)(void*, std::int32_t, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1), value(2));
	case 4: return reinterpret_cast<Ret (*)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1), value(2), value(3));
	case 5: return reinterpret_cast<Ret (*)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1), value(2), value(3), value(4));
	default: throw std::runtime_error("AsmJit test session supports at most five i32 parameters");
	}
}

void callI32VoidFunction(void* function, void* vm, const std::vector<const void*>& args)
{
	auto value = [&](std::size_t i) { return *static_cast<const std::int32_t*>(args[i]); };
	switch (args.size()) {
	case 0: reinterpret_cast<void (*)(void*)>(function)(vm); return;
	case 1: reinterpret_cast<void (*)(void*, std::int32_t)>(function)(vm, value(0)); return;
	case 2: reinterpret_cast<void (*)(void*, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1)); return;
	case 3: reinterpret_cast<void (*)(void*, std::int32_t, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1), value(2)); return;
	case 4: reinterpret_cast<void (*)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1), value(2), value(3)); return;
	case 5: reinterpret_cast<void (*)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t)>(function)(vm, value(0), value(1), value(2), value(3), value(4)); return;
	default: throw std::runtime_error("AsmJit test session supports at most five i32 parameters");
	}
}

class AsmJitSession final : public Session
{
public:
	AsmJitSession() : compiler_(context_) {}
	WASM::ModuleInstantiator& instantiator() override { return compiler_; }
	std::unique_ptr<LaneWiseOp> createLaneWiseOp(bool) override {
		throw std::runtime_error("AsmJit lane-wise operation is not exposed by the module compiler");
	}
	AggregateLayout aggregateLayout(const std::vector<WASM::StorageType>&) override { return {}; }
	bool invoke(const WASM::ModuleInstance& instance, const WASM::Callable& callable,
			const WASM::FuncType& type, void* vmContext, const std::vector<const void*>& args,
			void* result, std::size_t capacity, bool& trapped, WASM::TrapKind& trapKind,
			std::string& trapMessage) override
	{
		(void)instance; trapped = false;
		if (callable.fnPtr == nullptr || args.size() != type.params.size() || type.results.size() > 1) return false;
		for (const auto& p : type.params) if (p.val.opcode != WASM::ValueTypeCode::I32) return false;
		if (!type.results.empty() && type.results.front().val.opcode != WASM::ValueTypeCode::I32) return false;
		if (type.results.empty()) { if (result != nullptr) return false; }
		else if (result == nullptr || capacity < sizeof(std::int32_t)) return false;
		{
			WASM::CallDepthScope depth;
			if (type.results.empty()) callI32VoidFunction(callable.fnPtr, vmContext, args);
			else { const std::int32_t value = callI32Function<std::int32_t>(callable.fnPtr, vmContext, args); std::memcpy(result, &value, sizeof(value)); }
		}
		const WASM::TrapInfo info = WASM::takePendingTrap();
		if (info.trapped) {
			trapped = true; trapKind = info.kind; trapMessage = info.message != nullptr ? info.message : "";
			WASM::emitTrapDiagnostic(info.kind, info.message, info.functionIndex, info.instructionOffset, info.functionName);
		}
		return true;
	}
private:
	AsmJitBackend::Context context_;
	AsmJitBackend::ModuleCompiler compiler_;
};
class LibJitLaneWiseOp final : public LaneWiseOp
{
public:
	explicit LibJitLaneWiseOp(bool multiply) { build(multiply); }
	~LibJitLaneWiseOp() override { jit_context_destroy(context_); }
	void run(const void* a, const void* b, void* out) const override { entry_(a, b, out); }

private:
	using Entry = void (*)(const void*, const void*, void*);
	void build(bool multiply)
	{
		context_ = jit_context_create();
		jit_context_build_start(context_);
		jit_type_t params[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
		jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 3, 1);
		jit_function_t function = jit_function_create(context_, signature);
		jit_value_t a = jit_value_get_param(function, 0);
		jit_value_t b = jit_value_get_param(function, 1);
		jit_value_t out = jit_value_get_param(function, 2);
		for (int lane = 0; lane < 4; ++lane) {
			const jit_nint offset = lane * 4;
			jit_value_t va = jit_insn_load_relative(function, a, offset, jit_type_int);
			jit_value_t vb = jit_insn_load_relative(function, b, offset, jit_type_int);
			jit_insn_store_relative(function, out, offset,
					multiply ? jit_insn_mul(function, va, vb)
							 : jit_insn_add(function, va, vb));
		}
		jit_insn_return(function, nullptr);
		jit_function_compile(function);
		jit_context_build_end(context_);
		entry_ = reinterpret_cast<Entry>(jit_function_to_closure(function));
	}

	Entry entry_ = nullptr;
	jit_context_t context_ = nullptr;
};

class LibJitSession final : public Session
{
public:
	LibJitSession()
		: compiler_(context_.rawContext())
	{
	}

	WASM::ModuleInstantiator& instantiator() override { return compiler_; }

	std::unique_ptr<LaneWiseOp> createLaneWiseOp(bool multiply) override
	{
		return std::make_unique<LibJitLaneWiseOp>(multiply);
	}

	AggregateLayout aggregateLayout(
			const std::vector<WASM::StorageType>& fields) override
	{
		AggregateLayout layout;
		if (fields.empty())
			return layout;
		jit_type_t tuple = context_.getTranslator().translateStruct(fields);
		if (tuple == nullptr)
			return layout;
		layout.size = static_cast<std::size_t>(jit_type_get_size(tuple));
		layout.alignment = static_cast<std::size_t>(jit_type_get_alignment(tuple));
		layout.fields.reserve(fields.size());
		for (std::size_t i = 0; i < fields.size(); ++i) {
			const jit_type_t field = jit_type_get_field(tuple, static_cast<unsigned>(i));
			layout.fields.push_back({
				static_cast<std::size_t>(jit_type_get_offset(tuple, static_cast<unsigned>(i))),
				static_cast<std::size_t>(jit_type_get_size(field)),
				static_cast<std::size_t>(jit_type_get_alignment(field))
			});
		}
		return layout;
	}

	bool invoke(const WASM::ModuleInstance& instance,
			const WASM::Callable& callable,
			const WASM::FuncType& functionType,
			void* context,
			const std::vector<const void*>& arguments,
			void* result,
			std::size_t resultCapacity,
			bool& trapped,
			WASM::TrapKind& trapKind,
			std::string& trapMessage) override;

private:
	LibJIT::Context context_;
	LibJIT::ModuleCompiler compiler_;
};

bool LibJitSession::invoke(const WASM::ModuleInstance& instance,
		const WASM::Callable& callable,
		const WASM::FuncType& functionType,
		void* context,
		const std::vector<const void*>& arguments,
		void* result,
		std::size_t resultCapacity,
		bool& trapped,
		WASM::TrapKind& trapKind,
		std::string& trapMessage)
{
	(void)instance;
	const bool multiResult = functionType.results.size() > 1;
	if (multiResult && result == nullptr)
		return false;
	if (result != nullptr && resultCapacity < 16)
		return false;

	// Slot 0 is the implicit VMContext* of every compiled function.
	void* vm = context;
	std::vector<void*> jitArguments;
	jitArguments.reserve(1 + arguments.size() + (multiResult ? 1 : 0));
	jitArguments.push_back(&vm);
	for (const void* argument : arguments)
		jitArguments.push_back(const_cast<void*>(argument));

	jit_type_t tupleType = nullptr;
	void* tupleAddress = result;
	if (multiResult) {
		tupleType = context_.getTranslator().translateStruct(functionType.results);
		if (tupleType == nullptr)
			return false;
		jitArguments.push_back(&tupleAddress);
	}

	// The v128-only path bypasses `jit_apply`, whose third stack-passed
	// aggregate is damaged on this LibJIT; the generated code itself uses the
	// ordinary host C ABI.
	const auto v128AbiCall = [&]() {
		if (multiResult || functionType.params.empty() || functionType.params.size() > 4)
			return false;
		for (const auto& param : functionType.params)
			if (param.val.opcode != WASM::ValueTypeCode::V128)
				return false;
		WASM::ValueTypeCode resultCode = WASM::ValueTypeCode::Void;
		if (functionType.results.size() == 1) {
			resultCode = functionType.results[0].val.opcode;
			if (resultCode != WASM::ValueTypeCode::V128
					&& resultCode != WASM::ValueTypeCode::I32
					&& resultCode != WASM::ValueTypeCode::I64
					&& resultCode != WASM::ValueTypeCode::F32
					&& resultCode != WASM::ValueTypeCode::F64)
				return false;
		} else if (functionType.results.size() > 1) {
			return false;
		}

		AbiV128 v128Args[4];
		for (std::size_t i = 0; i < functionType.params.size(); ++i)
			std::memcpy(&v128Args[i], arguments[i], sizeof(AbiV128));

		const std::size_t count = functionType.params.size();
		void* fnPtr = callable.fnPtr;
		switch (resultCode) {
		case WASM::ValueTypeCode::V128: {
			const AbiV128 value = callWithV128Params<AbiV128>(fnPtr, vm, v128Args, count);
			std::memcpy(result, &value, sizeof(value));
			break;
		}
		case WASM::ValueTypeCode::I32: {
			const int32_t value = callWithV128Params<int32_t>(fnPtr, vm, v128Args, count);
			std::memcpy(result, &value, sizeof(value));
			break;
		}
		case WASM::ValueTypeCode::I64: {
			const int64_t value = callWithV128Params<int64_t>(fnPtr, vm, v128Args, count);
			std::memcpy(result, &value, sizeof(value));
			break;
		}
		case WASM::ValueTypeCode::F32: {
			const float value = callWithV128Params<float>(fnPtr, vm, v128Args, count);
			std::memcpy(result, &value, sizeof(value));
			break;
		}
		case WASM::ValueTypeCode::F64: {
			const double value = callWithV128Params<double>(fnPtr, vm, v128Args, count);
			std::memcpy(result, &value, sizeof(value));
			break;
		}
		default:
			callWithV128ParamsVoid(fnPtr, vm, v128Args, count);
			break;
		}
		return true;
	};

	// The generated code reports a trap through the engine's pending slot; the
	// harness turns that into its own trapped/kind/message result. A C++ frame
	// below generated code must not throw (the engine would have to unwind a
	// generated frame), which is exactly why the harness reads the slot instead
	// of relying on an exception here.
	const auto collectTrap = [&]() {
		const WASM::TrapInfo info = WASM::takePendingTrap();
		if (!info.trapped)
			return false;
		trapped = true;
		trapKind = info.kind;
		trapMessage = info.message != nullptr ? info.message : "";
		// This harness reads the slot directly instead of calling `checkTrap`,
		// so it is the host boundary: record the trap here exactly once.
		WASM::emitTrapDiagnostic(info.kind, info.message, info.functionIndex,
				info.instructionOffset, info.functionName);
		return true;
	};

	{
		WASM::CallDepthScope depth;
		if (v128AbiCall()) {
			collectTrap();
			return true;
		}

		const jit_type_t signature =
				context_.getTranslator().translateFunctionSignature(functionType);
		const unsigned int numArgs = static_cast<unsigned int>(jitArguments.size());
		jit_apply(signature, callable.fnPtr, jitArguments.data(), numArgs,
				multiResult ? nullptr : result);
	}
	collectTrap();
	return true;
}

} // namespace

std::unique_ptr<Session> create()
{
	const char* requested = std::getenv("WASMJIT_BACKEND");
	const std::string name = requested == nullptr ? "libjit" : requested;
	if (name.empty() || name == "libjit")
		return std::make_unique<LibJitSession>();
	if (name == "asmjit")
		return std::make_unique<AsmJitSession>();
	throw std::runtime_error("unknown WASMJIT_BACKEND '" + name
			+ "' (available test backends: 'libjit', 'asmjit')");
}

} // namespace TestBackend
