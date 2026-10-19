#include "ComponentRuntime.hpp"
#include "../WasmBase/WasmRegistryImportResolver.hpp"
#include "../WasmBase/WasmStore.hpp"
#include <ffi.h>
#include <stdexcept>

namespace LibJIT {
namespace {

union AbiSlot {
	int32_t i32;
	int64_t i64;
	float f32;
	double f64;
	uint64_t raw;
};

ffi_type* ffiType(WASM::AbiCoreType type)
{
	switch (type) {
	case WASM::AbiCoreType::I32: return &ffi_type_sint32;
	case WASM::AbiCoreType::I64: return &ffi_type_sint64;
	case WASM::AbiCoreType::F32: return &ffi_type_float;
	case WASM::AbiCoreType::F64: return &ffi_type_double;
	}
	throw std::runtime_error("component runtime: unknown ABI core type");
}

WASM::ValueTypeCode valueKind(WASM::AbiCoreType type)
{
	switch (type) {
	case WASM::AbiCoreType::I32: return WASM::ValueTypeCode::I32;
	case WASM::AbiCoreType::I64: return WASM::ValueTypeCode::I64;
	case WASM::AbiCoreType::F32: return WASM::ValueTypeCode::F32;
	case WASM::AbiCoreType::F64: return WASM::ValueTypeCode::F64;
	}
	throw std::runtime_error("component runtime: unknown ABI core type");
}

WASM::CanonicalAbiOptions canonicalOptions(const WASM::Canon& canon)
{
	WASM::CanonicalAbiOptions options;
	for (const WASM::CanonOpt& opt : canon.opts) {
		switch (opt.kind) {
		case WASM::CanonOptKind::Utf8:
			options.stringEncoding =
					WASM::CanonicalAbiOptions::StringEncoding::Utf8;
			break;
		case WASM::CanonOptKind::Utf16:
			options.stringEncoding =
					WASM::CanonicalAbiOptions::StringEncoding::Utf16;
			break;
		case WASM::CanonOptKind::Latin1Utf16:
			options.stringEncoding =
					WASM::CanonicalAbiOptions::StringEncoding::Latin1Utf16;
			break;
		case WASM::CanonOptKind::Memory:
			options.memory = opt.index;
			break;
		case WASM::CanonOptKind::Realloc:
			options.realloc = opt.index;
			break;
		case WASM::CanonOptKind::PostReturn:
			options.postReturn = opt.index;
			break;
		case WASM::CanonOptKind::Async:
			options.async = true;
			break;
		case WASM::CanonOptKind::Callback:
			break;
		}
	}
	if (canon.async)
		options.async = *canon.async;
	return options;
}

struct LowerData {
	ComponentFunction function;
	ffi_cif* cif = nullptr;
	std::vector<ffi_type*> argumentTypes;
};

void lowerCallback(ffi_cif*, void* result, void** args, void* userData)
{
	auto* data = static_cast<LowerData*>(userData);
	std::vector<WASM::Value> input(data->function.params.size());
	for (std::size_t i = 0; i < input.size(); ++i) {
		switch (data->function.params[i]) {
		case WASM::AbiCoreType::I32:
			input[i].kind = WASM::ValueTypeCode::I32;
			input[i].i32 = *static_cast<int32_t*>(args[i + 1]);
			break;
		case WASM::AbiCoreType::I64:
			input[i].kind = WASM::ValueTypeCode::I64;
			input[i].i64 = *static_cast<int64_t*>(args[i + 1]);
			break;
		case WASM::AbiCoreType::F32:
			input[i].kind = WASM::ValueTypeCode::F32;
			input[i].f32 = *static_cast<float*>(args[i + 1]);
			break;
		case WASM::AbiCoreType::F64:
			input[i].kind = WASM::ValueTypeCode::F64;
			input[i].f64 = *static_cast<double*>(args[i + 1]);
			break;
		}
	}
	std::vector<WASM::Value> output(data->function.results.size());
	data->function.call(input, output);
	if (data->function.results.empty())
		return;
	switch (data->function.results.front()) {
	case WASM::AbiCoreType::I32:
		*static_cast<int32_t*>(result) = output[0].i32;
		break;
	case WASM::AbiCoreType::I64:
		*static_cast<int64_t*>(result) = output[0].i64;
		break;
	case WASM::AbiCoreType::F32:
		*static_cast<float*>(result) = output[0].f32;
		break;
	case WASM::AbiCoreType::F64:
		*static_cast<double*>(result) = output[0].f64;
		break;
	}
}

} // namespace

ComponentRuntime::ComponentRuntime(jit_context_t context,
		const WASM::Component& component, ComponentImports imports)
	: compiler_(context), component_(component), imports_(std::move(imports))
{
	build();
}

ComponentRuntime::~ComponentRuntime()
{
	for (void* closure : lowerClosures_)
		if (closure)
			ffi_closure_free(closure);
	for (void* cif : lowerCifs_)
		delete static_cast<ffi_cif*>(cif);
	for (void* data : lowerData_)
		delete static_cast<LowerData*>(data);
}

void ComponentRuntime::build()
{
	for (const WASM::ComponentDefinition& definition : component_.definitions) {
		switch (definition.kind) {
		case WASM::ComponentDefinitionKind::CoreInstance:
			processCoreInstance(component_.coreInstances[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Alias:
			processAlias(component_.aliases[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Canon:
			processCanon(component_.canons[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Import:
			processImport(component_.imports[definition.index]);
			break;
		case WASM::ComponentDefinitionKind::Export:
			processExport(component_.exports[definition.index]);
			break;
		default:
			break;
		}
	}
}

void ComponentRuntime::processCoreInstance(const WASM::CoreInstance& instance)
{
	if (instance.kind != WASM::CoreInstance::Kind::Instantiate)
		throw std::runtime_error(
				"component runtime: inline core instances are not supported");
	if (instance.moduleIndex >= component_.coreModules.size())
		throw std::runtime_error(
				"component runtime: core instance module index out of bounds");
	WASM::RegistryImportResolver resolver;
	for (const WASM::CoreInstantiateArg& arg : instance.args) {
		if (arg.instanceIndex >= coreInstances_.size())
			throw std::runtime_error(
					"component runtime: core instantiate argument index out of bounds");
		coreInstances_[arg.instanceIndex]->registerExports(resolver, arg.name);
	}
	auto moduleInstance = compiler_.instantiate(
			*component_.coreModules[instance.moduleIndex], resolver);
	if (!moduleInstance)
		throw std::runtime_error("component runtime: core instantiation failed");
	coreInstances_.push_back(std::move(moduleInstance));
}

void ComponentRuntime::processAlias(const WASM::Alias& alias)
{
	if (alias.kind != WASM::Alias::Kind::CoreExport)
		return;
	if (alias.sort.sort != WASM::ComponentSort::Core || !alias.sort.core
			|| alias.sort.core->sort != WASM::CoreSort::Func)
		return;
	if (alias.instanceIndex >= coreInstances_.size())
		throw std::runtime_error(
				"component runtime: core alias instance index out of bounds");
	const std::optional<WASM::Callable> callable =
			coreInstances_[alias.instanceIndex]->exportedFunction(alias.name);
	if (!callable)
		throw std::runtime_error(
				"component runtime: core export is not a function");
	coreFuncs_.push_back(*callable);
}

void ComponentRuntime::processCanon(const WASM::Canon& canon)
{
	if (canon.kind == WASM::Canon::Kind::Lower) {
		if (canon.funcIndex >= componentFuncs_.size())
			throw std::runtime_error(
					"component runtime: canon lower function index out of bounds");
		auto data = std::make_unique<LowerData>();
		data->function = componentFuncs_[canon.funcIndex];
		if (data->function.results.size() > 1)
			throw std::runtime_error(
					"component runtime: multi-value core results are not supported");
		data->argumentTypes.push_back(&ffi_type_pointer);
		for (WASM::AbiCoreType type : data->function.params)
			data->argumentTypes.push_back(ffiType(type));
		data->cif = new ffi_cif;
		ffi_type* returnType = data->function.results.empty()
				? &ffi_type_void : ffiType(data->function.results.front());
		if (ffi_prep_cif(data->cif, FFI_DEFAULT_ABI,
				static_cast<unsigned>(data->argumentTypes.size()), returnType,
				data->argumentTypes.data()) != FFI_OK)
			throw std::runtime_error("component runtime: ffi_prep_cif failed");
		void* code = nullptr;
		ffi_closure* closure = static_cast<ffi_closure*>(
				ffi_closure_alloc(sizeof(ffi_closure), &code));
		if (!closure || !code
				|| ffi_prep_closure_loc(closure, data->cif, lowerCallback,
						data.get(), code) != FFI_OK) {
			if (closure)
				ffi_closure_free(closure);
			delete data->cif;
			data->cif = nullptr;
			throw std::runtime_error(
					"component runtime: ffi closure allocation failed");
		}
		WASM::Callable callable;
		callable.fnPtr = code;
		callable.context = nullptr;
		callable.localTypeIdx = 0;
		callable.typeId = WASM::TypeId{WASM::TypeId::kNone};
		coreFuncs_.push_back(callable);
		lowerClosures_.push_back(closure);
		lowerCifs_.push_back(data->cif);
		lowerData_.push_back(data.release());
		return;
	}
	if (canon.kind != WASM::Canon::Kind::Lift)
		throw std::runtime_error(
				"component runtime: this canonical builtin is not supported");
	if (canon.coreFuncIndex >= coreFuncs_.size())
		throw std::runtime_error(
				"component runtime: canon lift core function index out of bounds");
	const WASM::TypeId typeId = component_.typeId(canon.typeIndex);
	if (typeId.isNone())
		throw std::runtime_error(
				"component runtime: canon lift type has no closed identity");
	const WASM::CanonicalAbiOptions options = canonicalOptions(canon);
	const WASM::CanonicalAbiSignature* abi =
			WASM::Store::global().types().store().canonicalAbi(typeId, options);
	if (!abi)
		throw std::runtime_error(
				"component runtime: canon lift type is outside the flat ABI subset");

	ComponentFunction function;
	function.typeId = typeId;
	function.params = abi->flatParams;
	function.results = abi->flatResults;
	const WASM::Callable callable = coreFuncs_[canon.coreFuncIndex];
	function.call = [this, callable, function](std::span<const WASM::Value> args,
			std::span<WASM::Value> results) {
		invokeCore(callable, function, args, results);
	};
	componentFuncs_.push_back(std::move(function));
}

void ComponentRuntime::processImport(const WASM::ImportDecl& importDecl)
{
	if (importDecl.type.kind != WASM::ExternType::Kind::Func)
		throw std::runtime_error(
				"component runtime: only function component imports are supported");
	const auto found = imports_.find(importDecl.name.name);
	if (found == imports_.end())
		throw std::runtime_error(
				"component runtime: missing host import `" + importDecl.name.name + "`");
	const WASM::TypeId typeId = component_.typeId(importDecl.type.typeIndex);
	if (typeId.isNone())
		throw std::runtime_error(
				"component runtime: imported function type has no closed identity");
	const WASM::CanonicalAbiSignature* abi =
			WASM::Store::global().types().store().canonicalAbi(typeId, {});
	if (!abi)
		throw std::runtime_error(
				"component runtime: imported function type is outside the flat ABI subset");
	if (found->second.params != abi->flatParams
			|| found->second.results != abi->flatResults)
		throw std::runtime_error(
				"component runtime: host function signature does not match import");
	ComponentFunction function = found->second;
	function.typeId = typeId;
	componentFuncs_.push_back(std::move(function));
}

void ComponentRuntime::processExport(const WASM::ComponentExport& exportDecl)
{
	if (exportDecl.sortIndex.sort != WASM::ComponentSort::Func)
		return;
	if (exportDecl.sortIndex.index >= componentFuncs_.size())
		throw std::runtime_error(
				"component runtime: function export index out of bounds");
	exportedFuncs_[exportDecl.name.name] = exportDecl.sortIndex.index;
}

std::optional<ComponentFunction> ComponentRuntime::exportedFunction(
		std::string_view name) const
{
	const auto found = exportedFuncs_.find(std::string(name));
	if (found == exportedFuncs_.end())
		return std::nullopt;
	return componentFuncs_[found->second];
}

WASM::Callable ComponentRuntime::coreFunction(std::size_t index) const
{
	if (index >= coreFuncs_.size())
		throw std::out_of_range("component runtime: core function index out of bounds");
	return coreFuncs_[index];
}

void ComponentRuntime::invokeCore(const WASM::Callable& callable,
		const ComponentFunction& function,
		std::span<const WASM::Value> args,
		std::span<WASM::Value> results) const
{
	if (args.size() != function.params.size())
		throw std::runtime_error("component runtime: argument count mismatch");
	if (results.size() != function.results.size())
		throw std::runtime_error("component runtime: result count mismatch");
	if (function.results.size() > 1)
		throw std::runtime_error(
				"component runtime: multi-value core results are not supported");
	if (!callable.fnPtr)
		throw std::runtime_error("component runtime: null core function pointer");

	std::vector<ffi_type*> argumentTypes;
	argumentTypes.reserve(function.params.size() + 1);
	argumentTypes.push_back(&ffi_type_pointer);
	std::vector<AbiSlot> slots(function.params.size());
	std::vector<void*> argumentValues;
	argumentValues.reserve(function.params.size() + 1);
	WASM::VMContext* context = callable.context;
	argumentValues.push_back(&context);
	for (std::size_t i = 0; i < function.params.size(); ++i) {
		argumentTypes.push_back(ffiType(function.params[i]));
		switch (function.params[i]) {
		case WASM::AbiCoreType::I32: slots[i].i32 = args[i].i32; break;
		case WASM::AbiCoreType::I64: slots[i].i64 = args[i].i64; break;
		case WASM::AbiCoreType::F32: slots[i].f32 = args[i].f32; break;
		case WASM::AbiCoreType::F64: slots[i].f64 = args[i].f64; break;
		}
		argumentValues.push_back(&slots[i]);
	}

	ffi_cif cif;
	ffi_type* returnType = function.results.empty()
			? &ffi_type_void : ffiType(function.results.front());
	if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
			static_cast<unsigned>(argumentValues.size()), returnType,
			argumentTypes.data()) != FFI_OK)
		throw std::runtime_error("component runtime: ffi_prep_cif failed");
	AbiSlot result{};
	ffi_call(&cif, FFI_FN(callable.fnPtr), function.results.empty()
			? nullptr : &result, argumentValues.data());
	if (!function.results.empty()) {
		results[0].kind = valueKind(function.results.front());
		switch (function.results.front()) {
		case WASM::AbiCoreType::I32: results[0].i32 = result.i32; break;
		case WASM::AbiCoreType::I64: results[0].i64 = result.i64; break;
		case WASM::AbiCoreType::F32: results[0].f32 = result.f32; break;
		case WASM::AbiCoreType::F64: results[0].f64 = result.f64; break;
		}
	}
}

} // namespace LibJIT
