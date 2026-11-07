#include "WasmTailTrampoline.hpp"

#include "WasmModuleInstance.hpp"
#include "WasmTrap.hpp"
#include "WasmValue.hpp"

#include <ffi.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace WASM {
namespace {

//! Host-ABI type of one Wasm storage type, or null for a value the uniform
//! slot representation does not carry (`v128`).
ffi_type* ffiTypeFor(const StorageType& storage) noexcept
{
	if (storage.isPacked)
		return &ffi_type_sint32;
	switch (storage.val.opcode) {
	case ValueTypeCode::I32: return &ffi_type_sint32;
	case ValueTypeCode::I64: return &ffi_type_sint64;
	case ValueTypeCode::F32: return &ffi_type_float;
	case ValueTypeCode::F64: return &ffi_type_double;
	case ValueTypeCode::V128: return nullptr;
	default: return &ffi_type_pointer; // references
	}
}

//! Bytes the driver copies out of `TailCallState::results` for one result.
std::size_t resultBytes(const StorageType& storage) noexcept
{
	if (storage.isPacked)
		return 4;
	switch (storage.val.opcode) {
	case ValueTypeCode::I32:
	case ValueTypeCode::F32: return 4;
	default: return 8;
	}
}

//! One host signature shared by a driver (raw entry) and a type thunk.
struct CallShape {
	ffi_cif cif {};
	std::vector<ffi_type*> argTypes;
	std::size_t paramCount = 0;
	bool single = false;
	bool multi = false;
	std::size_t resultSize = 0;
};

//! Builds the host call shape of `ft`; false when it is outside the slot model.
bool buildCallShape(const FuncType& ft, CallShape& shape)
{
	shape.paramCount = ft.params.size();
	shape.argTypes.push_back(&ffi_type_pointer); // VMContext*
	for (const StorageType& param : ft.params) {
		ffi_type* type = ffiTypeFor(param);
		if (type == nullptr)
			return false;
		shape.argTypes.push_back(type);
	}
	shape.single = ft.results.size() == 1;
	shape.multi = ft.results.size() > 1;
	if (shape.multi)
		shape.argTypes.push_back(&ffi_type_pointer); // result buffer
	ffi_type* resultType = &ffi_type_void;
	if (shape.single) {
		resultType = ffiTypeFor(ft.results.front());
		if (resultType == nullptr)
			return false;
		shape.resultSize = resultBytes(ft.results.front());
	}
	return ffi_prep_cif(&shape.cif, FFI_DEFAULT_ABI,
			static_cast<unsigned>(shape.argTypes.size()), resultType,
			shape.argTypes.data()) == FFI_OK;
}

struct TypeThunk {
	CallShape shape;
	ffi_closure* closure = nullptr;
	void* code = nullptr;
};

struct FunctionDriver {
	CallShape shape;
	void* raw = nullptr;
	ffi_cif* thunkCif = nullptr;
	ffi_closure* closure = nullptr;
	void* code = nullptr;
};

class TrampolineTable {
public:
	TrampolineTable()
	{
		// The thunk ABI is uniform: (ctx, fn, slots, results, buffer).
		thunkArgs = {&ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer,
				&ffi_type_pointer, &ffi_type_pointer};
		ffi_prep_cif(&thunkCif, FFI_DEFAULT_ABI, 5, &ffi_type_void,
				thunkArgs.data());
	}

	~TrampolineTable()
	{
		for (std::unique_ptr<TypeThunk>& thunk : thunks)
			if (thunk && thunk->closure != nullptr)
				ffi_closure_free(thunk->closure);
		for (std::unique_ptr<FunctionDriver>& driver : drivers)
			if (driver && driver->closure != nullptr)
				ffi_closure_free(driver->closure);
	}

	std::vector<std::unique_ptr<TypeThunk>> thunks; //!< Indexed by type index.
	std::vector<std::unique_ptr<FunctionDriver>> drivers;
	ffi_cif thunkCif {};
	std::vector<ffi_type*> thunkArgs;
};

//! The thunk handler: loads typed arguments out of the uniform slots and calls
//! the raw entry (or a native import) through the host C ABI.
void thunkHandler(ffi_cif*, void*, void** args, void* userData)
{
	TypeThunk* thunk = static_cast<TypeThunk*>(userData);
	void* ctx = *static_cast<void**>(args[0]);
	void* fn = *static_cast<void**>(args[1]);
	auto* slots = *static_cast<std::uint64_t**>(args[2]);
	void* results = *static_cast<void**>(args[3]);
	void* buffer = *static_cast<void**>(args[4]);
	if (fn == nullptr)
		return;
	std::vector<void*> callArgs(thunk->shape.argTypes.size(), nullptr);
	callArgs[0] = &ctx;
	for (std::size_t i = 0; i < thunk->shape.paramCount; ++i)
		callArgs[1 + i] = reinterpret_cast<char*>(slots) + i * kTailCallSlotBytes;
	if (thunk->shape.multi)
		callArgs[1 + thunk->shape.paramCount] = &buffer;
	void* result = thunk->shape.single ? results : nullptr;
	ffi_call(&thunk->shape.cif, FFI_FN(fn), result, callArgs.data());
}

//! The driver handler: calls the raw entry once, then re-dispatches every
//! pending tail call through the target's thunk until the chain ends.
void driverHandler(ffi_cif*, void* ret, void** args, void* userData)
{
	FunctionDriver* driver = static_cast<FunctionDriver*>(userData);
	TailCallState* state = currentTailCallState();
	TrapSlot* trap = wasm_trap_slot();
	state->pending = 0;
	state->resultBuffer = driver->shape.multi
			? *static_cast<void**>(args[1 + driver->shape.paramCount])
			: nullptr;
	ffi_call(&driver->shape.cif, FFI_FN(driver->raw),
			driver->shape.single ? ret : nullptr, args);
	bool dispatched = false;
	while (state->pending != 0 && trap->pending == 0) {
		Callable* target = state->target;
		if (target == nullptr || target->tailEntry == nullptr)
			break;
		void* fn = target->rawFnPtr != nullptr ? target->rawFnPtr : target->fnPtr;
		if (fn == nullptr)
			break;
		void* ctx = target->context != nullptr
				? static_cast<void*>(target->context)
				: *static_cast<void**>(args[0]);
		void* ctxValue = ctx;
		void* fnValue = fn;
		void* slots = state->args;
		void* results = state->results;
		void* buffer = state->resultBuffer;
		void* thunkArgs[5] = {&ctxValue, &fnValue, &slots, &results, &buffer};
		state->pending = 0;
		ffi_call(driver->thunkCif, FFI_FN(target->tailEntry), nullptr, thunkArgs);
		dispatched = true;
	}
	if (dispatched && driver->shape.single && trap->pending == 0)
		std::memcpy(ret, state->results, driver->shape.resultSize);
}

} // namespace


bool installTailCallTrampolines(const Module& module, ModuleInstanceInternals& internals)
{
	if (!module.hasTailCalls())
		return false;
	if (internals.internalCallables.size() != module.internalFunctionTypeIndices.size())
		return false;

	auto table = std::make_unique<TrampolineTable>();
	const auto thunkFor = [&](uint32_t typeIndex) -> TypeThunk* {
		if (typeIndex >= table->thunks.size())
			table->thunks.resize(typeIndex + 1);
		if (table->thunks[typeIndex])
			return table->thunks[typeIndex].get();
		const FuncType ft = module.funcTypeAt(typeIndex);
		if (!tailCallSlotsSupported(ft))
			return nullptr;
		auto thunk = std::make_unique<TypeThunk>();
		if (!buildCallShape(ft, thunk->shape))
			return nullptr;
		void* code = nullptr;
		thunk->closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &code));
		if (thunk->closure == nullptr
				|| ffi_prep_closure_loc(thunk->closure, &table->thunkCif,
						thunkHandler, thunk.get(), code) != FFI_OK)
			return nullptr;
		thunk->code = code;
		table->thunks[typeIndex] = std::move(thunk);
		return table->thunks[typeIndex].get();
	};

	// Build everything first; only then publish it to the callables, so a
	// failed closure does not leave half a trampoline installed.
	for (uint32_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i) {
		const uint32_t typeIndex = module.internalFunctionTypeIndices[i];
		const FuncType ft = module.funcTypeAt(typeIndex);
		if (!tailCallSlotsSupported(ft))
			return false;
		TypeThunk* thunk = thunkFor(typeIndex);
		if (thunk == nullptr || internals.internalCallables[i].fnPtr == nullptr)
			return false;
		auto driver = std::make_unique<FunctionDriver>();
		driver->raw = internals.internalCallables[i].fnPtr;
		driver->thunkCif = &table->thunkCif;
		if (!buildCallShape(ft, driver->shape))
			return false;
		void* code = nullptr;
		driver->closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &code));
		if (driver->closure == nullptr
				|| ffi_prep_closure_loc(driver->closure, &driver->shape.cif,
						driverHandler, driver.get(), code) != FFI_OK)
			return false;
		driver->code = code;
		table->drivers.push_back(std::move(driver));
	}

	for (uint32_t i = 0; i < module.importFunctions.size(); ++i) {
		const FuncType ft = module.funcTypeAt(module.importFunctions[i].typeIdx);
		if (!tailCallSlotsSupported(ft))
			return false;
		if (thunkFor(module.importFunctions[i].typeIdx) == nullptr)
			return false;
	}


	for (uint32_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i) {
		Callable& callable = internals.internalCallables[i];
		callable.rawFnPtr = callable.fnPtr;
		callable.fnPtr = table->drivers[i]->code;
		callable.tailEntry = table->thunks[module.internalFunctionTypeIndices[i]]->code;
	}
	for (uint32_t i = 0; i < module.importFunctions.size() && i < internals.importStorage.size(); ++i) {
		internals.importStorage[i].tailEntry =
				table->thunks[module.importFunctions[i].typeIdx]->code;
	}

	releaseTailCallTrampolines(internals);
	internals.tailTrampolines = table.release();
	return true;
}

void releaseTailCallTrampolines(ModuleInstanceInternals& internals) noexcept
{
	delete static_cast<TrampolineTable*>(internals.tailTrampolines);
	internals.tailTrampolines = nullptr;
}

} // namespace WASM

