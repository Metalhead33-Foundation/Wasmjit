#include "WasmCallCache.hpp"

#include "WasmBase/WasmBackendAbi.hpp"

#include "WasmModule.hpp"
#include "WasmStore.hpp"
#include "WasmType.hpp"
#include "WasmTypeStore.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#include <ffi.h>

namespace WASM {
namespace {

//! A 16-byte vector is four i32 cells in the engine's native ABI. libffi sees
//! the same register/stack classification as that four-cell aggregate.
ffi_type* v128Type()
{
	static ffi_type* const elements[] = {
		&ffi_type_uint32, &ffi_type_uint32, &ffi_type_uint32, &ffi_type_uint32,
		nullptr
	};
	static ffi_type type{16, 16, FFI_TYPE_STRUCT,
			const_cast<ffi_type**>(elements)};
	return &type;
}

ffi_type* ffiTypeFor(ValueTypeCode code)
{
	switch (code) {
	case ValueTypeCode::I32:
	case ValueTypeCode::S32:
	case ValueTypeCode::I8:
	case ValueTypeCode::I16:
		return &ffi_type_sint32;
	case ValueTypeCode::I64:
	case ValueTypeCode::S64:
		return &ffi_type_sint64;
	case ValueTypeCode::F32:
		return &ffi_type_float;
	case ValueTypeCode::F64:
		return &ffi_type_double;
	case ValueTypeCode::V128:
		return v128Type();
	case ValueTypeCode::Void:
		return &ffi_type_void;
	default:
		// Every reference kind, including stringref and its views, is an
		// opaque pointer-sized cell at the Callable ABI.
		return &ffi_type_pointer;
	}
}

ffi_type* ffiTypeFor(const CanonStorageType& storage)
{
	if (storage.isPacked)
		return &ffi_type_sint32;
	return ffiTypeFor(storage.value.opcode);
}

//! A `v128` spelled as one 16-byte aggregate (two `uint64` eightbytes).
//! libffi classifies this as INTEGER, INTEGER on SysV and passes it by reference
//! on MS x64 -- which is exactly what a backend that cannot use the canonical
//! two-word rule produces (`docs/backends/LIBJIT_ARGUMENT_DEFECT.md` §12).
ffi_type* ffiTypeForV128Aggregate()
{
	static ffi_type* elements[3] = {&ffi_type_uint64, &ffi_type_uint64, nullptr};
	static ffi_type aggregate {0, 0, FFI_TYPE_STRUCT, elements};
	return &aggregate;
}

ffi_type* ffiTypeFor(AbiCoreType type)
{
	switch (type) {
	case AbiCoreType::I32: return &ffi_type_sint32;
	case AbiCoreType::I64: return &ffi_type_sint64;
	case AbiCoreType::F32: return &ffi_type_float;
	case AbiCoreType::F64: return &ffi_type_double;
	}
	return &ffi_type_void;
}

void prepare(ffi_cif& cif, std::vector<ffi_type*>& arguments,
		ffi_type* resultType)
{
	if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
				static_cast<unsigned>(arguments.size()), resultType,
				arguments.data()) != FFI_OK)
		throw std::runtime_error("call cache: ffi_prep_cif failed");
}

} // namespace

ModuleCallCache::ModuleCallCache(ModuleInstance& instance)
	: instance_(&instance)
{
	const auto descriptors = instance.sourceModule().exportDescriptors();
	entries_.reserve(descriptors.size());
	for (const ExportDescriptor& descriptor : descriptors) {
		if (descriptor.type.kind != ExternalKind::Function
				|| descriptor.type.typeId.isNone())
			continue;
		const CoreTypeView view =
				instance.store().types().store().coreType(descriptor.type.typeId);
		if (!view.valid() || view.kind() != CanonTypeKind::Func)
			continue;
		const std::optional<Callable> callable =
				instance.exportedFunction(descriptor.name);
		if (!callable)
			continue;

		ModuleFfiEntry entry;
		entry.name = descriptor.name;
		entry.callable = *callable;
		entry.paramCount = view.funcParams().size();
		entry.multiResult = view.funcResults().size() > 1u;
		entry.argTypes.reserve(1u + entry.paramCount
				+ (entry.multiResult ? 1u : 0u));
		entry.argTypes.push_back(&ffi_type_pointer); // VMContext*
		for (const CanonStorageType& parameter : view.funcParams()) {
			entry.paramKinds.push_back(parameter.value.opcode);
			const std::uint32_t slots = argSlotCount(valueKindOf(parameter.value.opcode));
			if (slots == 1) {
				entry.argTypes.push_back(ffiTypeFor(parameter));
			} else if (instance.v128ParameterIsAggregate()) {
				// This backend spells a vector as one 16-byte aggregate, so the
				// cif must describe that shape or the call is misclassified on
				// MS x64 (docs/backends/LIBJIT_ARGUMENT_DEFECT.md §12).
				entry.argTypes.push_back(ffiTypeForV128Aggregate());
			} else {
				// `argSlotCount` decides how many C arguments a kind occupies; a vector
				// occupies two scalar words (decision note §28), because a 16-byte
				// aggregate is passed by reference on MS x64.
				for (std::uint32_t slot = 0; slot < slots; ++slot)
					entry.argTypes.push_back(&ffi_type_uint64);
			}
		}
		if (entry.multiResult)
			entry.argTypes.push_back(&ffi_type_pointer); // result buffer
		entry.returnType = view.funcResults().empty() || entry.multiResult
				? &ffi_type_void
				: ffiTypeFor(view.funcResults().front());
		auto cif = std::make_unique<ffi_cif>();
		prepare(*cif, entry.argTypes, entry.returnType);

		byName_.emplace(entry.name, entries_.size());
		entries_.push_back(std::move(entry));
		cifs_.push_back(cif.get());
		cif.release();
	}
}

ModuleCallCache::~ModuleCallCache()
{
	for (void* cif : cifs_)
		delete static_cast<ffi_cif*>(cif);
}

const ModuleFfiEntry* ModuleCallCache::find(std::string_view name) const
{
	const auto found = byName_.find(std::string(name));
	return found == byName_.end() ? nullptr : &entries_[found->second];
}

void ModuleCallCache::call(const ModuleFfiEntry& entry, void* result,
		void* const* args) const
{
	if (instance_ == nullptr || entry.callable.fnPtr == nullptr)
		throw std::runtime_error("module call cache: invalid entry");
	if (!entry.multiResult && entry.returnType != &ffi_type_void
			&& result == nullptr)
		throw std::runtime_error("module call cache: result storage is null");
	const auto found = std::find_if(entries_.begin(), entries_.end(),
			[&entry](const ModuleFfiEntry& candidate) { return &candidate == &entry; });
	if (found == entries_.end())
		throw std::runtime_error("module call cache: entry is not from this cache");
	const auto index = static_cast<std::size_t>(found - entries_.begin());
	ffi_cif* cif = static_cast<ffi_cif*>(cifs_[index]);
	std::vector<void*> arguments(entry.argTypes.size(), nullptr);
	void* context = entry.callable.context != nullptr
			? static_cast<void*>(entry.callable.context)
			: static_cast<void*>(instance_->context());
	arguments[0] = &context;
	std::size_t slot = 1; // argument 0 is the VMContext*
	for (std::size_t i = 0; i < entry.paramCount; ++i) {
		const std::uint32_t count = argSlotCount(valueKindOf(entry.paramKinds[i]));
		if (count == 1) {
			arguments[slot++] = args[i];
		} else {
			// A vector is handed over as its low word, then its high word.
			auto* bytes = static_cast<std::uint8_t*>(args[i]);
			for (std::uint32_t word = 0; word < count; ++word)
				arguments[slot++] = bytes + word * sizeof(std::uint64_t);
		}
	}
	void* resultBuffer = result;
	if (entry.multiResult)
		arguments.back() = &resultBuffer;

	// libffi's ffi_call stores an ffi_arg word for integer and floating-point
	// results smaller than a word. Give it word-sized scratch and narrow into
	// the caller's storage ourselves; writing 8 bytes into a 4-byte int32_t
	// would corrupt the neighbouring stack slot.
	ffi_arg wordResult = 0;
	const bool narrowed = !entry.multiResult
			&& entry.returnType != &ffi_type_void
			&& entry.returnType->type != FFI_TYPE_STRUCT
			&& entry.returnType->size <= sizeof(ffi_arg);
	{
		CallDepthScope depth;
		ffi_call(cif, FFI_FN(entry.callable.fnPtr),
				narrowed ? static_cast<void*>(&wordResult) : result,
				arguments.data());
	}
	if (narrowed && result != nullptr)
		std::memcpy(result, &wordResult, entry.returnType->size);
	checkTrap();
}

void ModuleCallCache::callWithResults(std::string_view name, void* result,
		void* const* args) const
{
	const ModuleFfiEntry* entry = find(name);
	if (entry == nullptr)
		throw std::runtime_error("module call cache: no export named "
				+ std::string(name));
	if (!entry->multiResult)
		throw std::runtime_error("module call cache: export is not multi-result: "
				+ std::string(name));
	call(*entry, result, args);
}

ComponentCallCache::ComponentCallCache(ComponentInstanceHandle& instance)
	: instance_(&instance)
{
	const std::vector<std::string> names = instance.exportedFunctionNames();
	entries_.reserve(names.size());
	for (const std::string& name : names) {
		const std::optional<ComponentFunction> function =
				instance.exportedFunction(name);
		if (!function || !function->call
				|| function->indirectParams || function->indirectResults)
			continue;

		ComponentFfiEntry entry;
		entry.name = name;
		entry.function = *function;
		entry.paramCount = entry.function.params.size();
		entry.multiResult = entry.function.results.size() > 1u;
		entry.argTypes.reserve(entry.paramCount);
		for (const AbiCoreType parameter : entry.function.params)
			entry.argTypes.push_back(ffiTypeFor(parameter));
		entry.returnType = entry.function.results.empty() || entry.multiResult
				? &ffi_type_void
				: ffiTypeFor(entry.function.results.front());

		byName_.emplace(entry.name, entries_.size());
		entries_.push_back(std::move(entry));
	}
}

const ComponentFfiEntry* ComponentCallCache::find(std::string_view name) const
{
	const auto found = byName_.find(std::string(name));
	return found == byName_.end() ? nullptr : &entries_[found->second];
}

std::vector<Value> ComponentCallCache::call(const ComponentFfiEntry& entry,
		std::span<const Value> args) const
{
	if (!entry.function.call)
		throw std::runtime_error("component call cache: entry is not callable");
	if (args.size() != entry.paramCount)
		throw std::runtime_error("component call cache: wrong argument count for "
				+ entry.name);
	std::vector<Value> results(entry.function.results.size());
	entry.function.call(args, results);
	checkTrap();
	return results;
}

} // namespace WASM
