#include "SljitBackendSession.hpp"

#include "SljitModuleCompiler.hpp"

#include "../WasmBase/WasmBackendAbi.hpp"
#include "../WasmBase/WasmDiagnostics.hpp"

#include <sljitLir.h>

#include <ffi.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace Sljit {

namespace {

//! `libffi` type of one Wasm value type, or null when the session cannot
//! marshal it. `v128` is the deliberate gap: a 16-byte aggregate does not fit
//! the provisional backend's slot model.
ffi_type* ffiTypeOf(WASM::ValueTypeCode code)
{
	switch (WASM::valueKindOf(code)) {
	case WASM::ValueKind::I32:  return &ffi_type_sint32;
	case WASM::ValueKind::I64:  return &ffi_type_sint64;
	case WASM::ValueKind::F32:  return &ffi_type_float;
	case WASM::ValueKind::F64:  return &ffi_type_double;
	case WASM::ValueKind::Ref:  return &ffi_type_pointer;
	case WASM::ValueKind::V128:
	case WASM::ValueKind::Void:
		return nullptr;
	}
	return nullptr;
}

//! A freshly emitted lane-wise `i32x4` operation. Owns its executable code and
//! releases it in the destructor, which is safe here because nothing else refers
//! to the block.
class LaneWiseOp final : public WASM::BackendLaneWiseOp
{
public:
	explicit LaneWiseOp(bool multiply) { build(multiply); }
	~LaneWiseOp() override
	{
		if (code_ != nullptr)
			sljit_free_code(code_, nullptr);
	}
	void run(const void* a, const void* b, void* out) const override { entry_(a, b, out); }

private:
	using Entry = void (*)(const void*, const void*, void*);

	void build(bool multiply)
	{
		sljit_compiler* compiler = sljit_create_compiler(nullptr);
		if (compiler == nullptr)
			throw std::runtime_error("SLJIT could not create the lane-wise compiler");

		// `(const void* a, const void* b, void* out)`: without the `_R` suffix
		// the three pointer arguments arrive in S0, S1 and S2.
		bool ok = sljit_emit_enter(compiler, 0, SLJIT_ARGS3V(P, P, P), 2, 3, 0)
				== SLJIT_SUCCESS;
		const sljit_s32 op = (multiply ? SLJIT_MUL : SLJIT_ADD) | SLJIT_32;
		for (int lane = 0; ok && lane < 4; ++lane) {
			const sljit_sw offset = lane * 4;
			ok = sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_R0, 0,
						SLJIT_MEM1(SLJIT_S0), offset) == SLJIT_SUCCESS
					&& sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_R1, 0,
						SLJIT_MEM1(SLJIT_S1), offset) == SLJIT_SUCCESS
					&& sljit_emit_op2(compiler, op, SLJIT_R0, 0, SLJIT_R0, 0,
						SLJIT_R1, 0) == SLJIT_SUCCESS
					&& sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S2),
						offset, SLJIT_R0, 0) == SLJIT_SUCCESS;
		}
		ok = ok && sljit_emit_return_void(compiler) == SLJIT_SUCCESS;

		void* code = ok ? sljit_generate_code(compiler, 0, nullptr) : nullptr;
		sljit_free_compiler(compiler);
		if (code == nullptr)
			throw std::runtime_error("SLJIT could not emit the lane-wise operation");

		code_ = code;
		entry_ = reinterpret_cast<Entry>(code);
	}

	Entry entry_ = nullptr; //!< Emitted entry point.
	void* code_ = nullptr; //!< Executable block owned by this object.
};

} // namespace

Session::Session()
	: compiler_(std::make_unique<ModuleCompiler>())
{
}

Session::~Session() = default;

WASM::ModuleInstantiator& Session::instantiator()
{
	return *compiler_;
}

WASM::BackendAggregateLayout Session::aggregateLayout(
		const std::vector<WASM::StorageType>& fields)
{
	WASM::BackendAggregateLayout layout;
	if (fields.empty())
		return layout;

	// The rule is the core's, not this backend's: `computeAggregateLayout`
	// implements docs/ABI.md "The result tuple layout" as PODs. A packed storage
	// type (i8/i16) is materialised as the i32 it widens to, which is the only
	// way it can reach a signature.
	std::vector<WASM::ValueKind> kinds;
	kinds.reserve(fields.size());
	for (const WASM::StorageType& field : fields) {
		kinds.push_back(field.isPacked ? WASM::ValueKind::I32
									   : WASM::valueKindOf(field.val.opcode));
	}

	std::vector<WASM::FieldLayout> fieldLayouts(fields.size());
	const WASM::AggregateLayout computed =
			WASM::computeAggregateLayout(kinds, fieldLayouts);
	layout.size = computed.size;
	layout.alignment = computed.alignment;
	layout.fields.reserve(computed.fieldCount);
	for (std::uint32_t i = 0; i < computed.fieldCount; ++i) {
		layout.fields.push_back(WASM::BackendFieldLayout{
			computed.fields[i].offset,
			computed.fields[i].size,
			computed.fields[i].alignment,
		});
	}
	return layout;
}

std::unique_ptr<WASM::BackendLaneWiseOp> Session::createLaneWiseOp(bool multiply)
{
	return std::make_unique<LaneWiseOp>(multiply);
}


bool Session::invoke(const WASM::ModuleInstance& instance,
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
	if (callable.fnPtr == nullptr)
		return false;
	// The emitter accepts three Wasm parameters plus the context, one result,
	// and no `v128`; `invoke` refuses exactly the same shapes.
	if (functionType.params.size() > 3 || functionType.results.size() > 1)
		return false;
	if (arguments.size() != functionType.params.size())
		return false;

	std::vector<ffi_type*> argumentTypes;
	std::vector<void*> argumentValues;
	argumentTypes.reserve(1 + functionType.params.size());
	argumentValues.reserve(1 + functionType.params.size());

	void* vmContext = context;
	argumentTypes.push_back(&ffi_type_pointer);
	argumentValues.push_back(&vmContext);

	for (std::size_t i = 0; i < functionType.params.size(); ++i) {
		ffi_type* type = ffiTypeOf(functionType.params[i].val.opcode);
		if (type == nullptr)
			return false;
		argumentTypes.push_back(type);
		argumentValues.push_back(const_cast<void*>(arguments[i]));
	}

	ffi_type* returnType = &ffi_type_void;
	if (functionType.results.size() == 1) {
		returnType = ffiTypeOf(functionType.results[0].val.opcode);
		if (returnType == nullptr)
			return false;
		const std::size_t needed = WASM::valueKindSize(
				WASM::valueKindOf(functionType.results[0].val.opcode));
		if (result == nullptr || resultCapacity < needed)
			return false;
	}

	ffi_cif interface{};
	if (ffi_prep_cif(&interface, FFI_DEFAULT_ABI,
			static_cast<unsigned>(argumentTypes.size()), returnType,
			argumentTypes.data()) != FFI_OK) {
		return false;
	}

	trapped = false;
	{
		// The scope marks this as a host boundary so a trap is collected here
		// rather than thrown through a generated frame.
		WASM::CallDepthScope depth;
		ffi_call(&interface, reinterpret_cast<void (*)()>(callable.fnPtr),
				returnType == &ffi_type_void ? nullptr : result, argumentValues.data());
	}

	const WASM::TrapInfo info = WASM::takePendingTrap();
	if (info.trapped) {
		trapped = true;
		trapKind = info.kind;
		trapMessage = info.message != nullptr ? info.message : "";
		// This session is the host boundary (it reads the slot directly), so it
		// records the diagnostic exactly once.
		WASM::emitTrapDiagnostic(info.kind, info.message, info.functionIndex,
				info.instructionOffset, info.functionName);
	}
	return true;
}

} // namespace Sljit
