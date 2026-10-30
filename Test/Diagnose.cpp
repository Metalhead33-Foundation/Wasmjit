#include "TestStore.hpp"
#include "Diagnose.hpp"
#include "TestBackend.hpp"

#include <Euphemy/Io/EuphFile.hpp>

#include "WasmBase/WasmDiagnostics.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmTrap.hpp"
#include "WasmBase/WasmType.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

const char* kindName(WASM::ExternalKind kind)
{
	switch (kind) {
	case WASM::ExternalKind::Function: return "func";
	case WASM::ExternalKind::Table:    return "table";
	case WASM::ExternalKind::Memory:   return "memory";
	case WASM::ExternalKind::Global:   return "global";
	case WASM::ExternalKind::Tag:      return "tag";
	case WASM::ExternalKind::Type:     return "type";
	}
	return "?";
}

const char* valueTypeName(WASM::ValueTypeCode code)
{
	switch (code) {
	case WASM::ValueTypeCode::I32: return "i32";
	case WASM::ValueTypeCode::I64: return "i64";
	case WASM::ValueTypeCode::F32: return "f32";
	case WASM::ValueTypeCode::F64: return "f64";
	case WASM::ValueTypeCode::V128: return "v128";
	case WASM::ValueTypeCode::I8: return "i8";
	case WASM::ValueTypeCode::I16: return "i16";
	default: break;
	}
	if (WASM::isReferenceType(code))
		return "ref";
	return "?";
}

std::string signature(const WASM::FuncType& type)
{
	const auto append = [](std::string& out, const std::vector<WASM::StorageType>& types) {
		for (std::size_t i = 0; i < types.size(); ++i) {
			if (i != 0)
				out += ", ";
			out += valueTypeName(types[i].val.opcode);
		}
	};
	std::string out = "(";
	append(out, type.params);
	out += ") -> (";
	append(out, type.results);
	out += ")";
	return out;
}

std::optional<std::uint32_t> functionTypeIndex(const WASM::Module& module,
		std::uint32_t functionIndex)
{
	const std::size_t imported = module.importFunctions.size();
	if (functionIndex < imported)
		return module.importFunctions[functionIndex].typeIdx;
	const std::size_t internal = functionIndex - imported;
	if (internal < module.internalFunctionTypeIndices.size())
		return module.internalFunctionTypeIndices[internal];
	return std::nullopt;
}

const WASM::Export* findExport(const WASM::Module& module, std::string_view name)
{
	for (const WASM::Export& exportEntry : module.exports)
		if (exportEntry.name == name)
			return &exportEntry;
	return nullptr;
}

// Every record currently in the ring, in the same text form the test harness
// uses. For a `--diagnose` run the ring is cleared at startup, so this covers
// exactly this attempt.
void printRecentDiagnostics()
{
	const std::span<const WASM::Diagnostic> records = WASM::recentDiagnostics();
	if (records.empty())
		return;
	std::fprintf(stderr, "diagnostics (%zu):\n", records.size());
	for (const WASM::Diagnostic& diagnostic : records) {
		const std::string text = WASM::formatDiagnostic(diagnostic);
		std::fprintf(stderr, "  %s\n", text.c_str());
	}
}

} // namespace

bool parseDiagnoseArgument(std::string_view argument, DiagnoseOptions& options)
{
	if (argument == "--diagnose") {
		options.specified = true;
		return true;
	}
	constexpr std::string_view kPathPrefix = "--diagnose=";
	if (argument.rfind(kPathPrefix, 0) == 0) {
		options.specified = true;
		options.path.assign(argument.substr(kPathPrefix.size()));
		return true;
	}
	constexpr std::string_view kInvokePrefix = "--invoke=";
	if (argument.rfind(kInvokePrefix, 0) == 0) {
		options.specified = true;
		options.invoke.assign(argument.substr(kInvokePrefix.size()));
		return true;
	}
	constexpr std::string_view kArgsPrefix = "--args=";
	if (argument.rfind(kArgsPrefix, 0) == 0) {
		options.specified = true;
		options.args.assign(argument.substr(kArgsPrefix.size()));
		return true;
	}
	return false;
}

namespace {

bool parseArguments(const std::string& text, const WASM::FuncType& type,
		std::vector<std::int32_t>& values, std::string& error)
{
	std::vector<std::string> parts;
	if (!text.empty()) {
		std::size_t start = 0;
		while (true) {
			const std::size_t comma = text.find(',', start);
			parts.push_back(text.substr(start, comma == std::string::npos
					? std::string::npos : comma - start));
			if (comma == std::string::npos)
				break;
			start = comma + 1;
		}
	}
	if (parts.size() != type.params.size()) {
		error = "expected " + std::to_string(type.params.size())
				+ " argument(s), got " + std::to_string(parts.size())
				+ "; signature is " + signature(type);
		return false;
	}
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (type.params[i].val.opcode != WASM::ValueTypeCode::I32) {
			error = "parameter " + std::to_string(i) + " is "
					+ valueTypeName(type.params[i].val.opcode)
					+ "; --args currently supports i32 parameters only";
			return false;
		}
		char* end = nullptr;
		const long parsed = std::strtol(parts[i].c_str(), &end, 0);
		if (end == parts[i].c_str() || *end != '\0') {
			error = "argument " + std::to_string(i) + " ('" + parts[i]
					+ "') is not a decimal integer";
			return false;
		}
		values.push_back(static_cast<std::int32_t>(parsed));
	}
	return true;
}

void printResults(const WASM::FuncType& type,
		const TestBackend::AggregateLayout& layout,
		const std::vector<std::uint8_t>& storage)
{
	if (type.results.empty()) {
		std::printf("result: (none)\n");
		return;
	}
	for (std::size_t i = 0; i < type.results.size(); ++i) {
		const std::size_t offset = type.results.size() > 1 && i < layout.fields.size()
				? layout.fields[i].offset : 0;
		const WASM::ValueTypeCode code = type.results[i].val.opcode;
		std::printf("result[%zu] %s = ", i, valueTypeName(code));
		switch (code) {
		case WASM::ValueTypeCode::I32: {
			std::int32_t value = 0;
			std::memcpy(&value, storage.data() + offset, sizeof(value));
			std::printf("%d\n", value);
			break;
		}
		case WASM::ValueTypeCode::I64: {
			std::int64_t value = 0;
			std::memcpy(&value, storage.data() + offset, sizeof(value));
			std::printf("%lld\n", static_cast<long long>(value));
			break;
		}
		case WASM::ValueTypeCode::F32: {
			float value = 0;
			std::memcpy(&value, storage.data() + offset, sizeof(value));
			std::printf("%.9g\n", static_cast<double>(value));
			break;
		}
		case WASM::ValueTypeCode::F64: {
			double value = 0;
			std::memcpy(&value, storage.data() + offset, sizeof(value));
			std::printf("%.17g\n", value);
			break;
		}
		default:
			std::printf("<%s>\n", valueTypeName(code));
			break;
		}
	}
}

int invokeExport(const WASM::Module& module, WASM::ModuleInstance& instance,
		TestBackend::Session& backend, const DiagnoseOptions& options)
{
	const WASM::Export* exportEntry = findExport(module, options.invoke);
	if (exportEntry == nullptr || exportEntry->kind != WASM::ExternalKind::Function) {
		std::fprintf(stderr, "diagnose: no function export named '%s'\n",
				options.invoke.c_str());
		return 1;
	}
	const std::optional<std::uint32_t> typeIndex =
			functionTypeIndex(module, exportEntry->index);
	if (!typeIndex.has_value()) {
		std::fprintf(stderr, "diagnose: export '%s' has no function type\n",
				options.invoke.c_str());
		return 1;
	}
	const WASM::FuncType type = module.funcTypeAt(*typeIndex);

	std::vector<std::int32_t> values;
	std::string error;
	if (!parseArguments(options.args, type, values, error)) {
		std::fprintf(stderr, "diagnose: %s\n", error.c_str());
		return 1;
	}
	std::vector<const void*> arguments;
	arguments.reserve(values.size());
	for (const std::int32_t& value : values)
		arguments.push_back(&value);

	const TestBackend::AggregateLayout layout = type.results.size() > 1
			? backend.aggregateLayout(type.results) : TestBackend::AggregateLayout{};
	const std::size_t capacity = std::max<std::size_t>(
			type.results.size() > 1 ? layout.size : 16, 16);
	std::vector<std::uint8_t> resultStorage(capacity, 0);

	const std::optional<WASM::Callable> callable =
			instance.exportedFunction(options.invoke);
	if (!callable.has_value()) {
		std::fprintf(stderr, "diagnose: export '%s' is not callable\n",
				options.invoke.c_str());
		return 1;
	}

	bool trapped = false;
	WASM::TrapKind trapKind = WASM::TrapKind::Internal;
	std::string trapMessage;
	if (!backend.invoke(instance, *callable, type, instance.context(), arguments,
			resultStorage.data(), resultStorage.size(), trapped, trapKind, trapMessage)) {
		std::fprintf(stderr, "diagnose: backend could not invoke '%s'\n",
				options.invoke.c_str());
		printRecentDiagnostics();
		return 1;
	}
	if (trapped) {
		std::fprintf(stderr, "trap: %s: %s\n", WASM::trapKindName(trapKind),
				trapMessage.c_str());
		printRecentDiagnostics();
		return 1;
	}
	printResults(type, layout, resultStorage);
	std::fflush(stdout);
	return 0;
}

int failWith(const std::string& what, const std::exception& error)
{
	std::fprintf(stderr, "diagnose: %s: %s\n", what.c_str(), error.what());
	printRecentDiagnostics();
	return 1;
}

} // namespace

int runDiagnose(const DiagnoseOptions& options)
{
	if (options.path.empty()) {
		std::fprintf(stderr, "diagnose: missing --diagnose=PATH\n");
		return 2;
	}

	// A diagnose run owns the ring for its duration; the harness sink (if any)
	// still receives the records for `--diagnostics`.
	WASM::clearDiagnostics();
	WASM::clearPendingTrap();

	WASM::Module module;
	try {
		Euph::Io::File file(options.path.c_str(), Elv::Io::Mode::READ);
		module.fromFile(file, testStore());
	} catch (const std::exception& error) {
		return failWith("cannot load " + options.path, error);
	}

	const std::vector<WASM::ImportDescriptor> imports = module.importDescriptors();
	const std::vector<WASM::ExportDescriptor> exports = module.exportDescriptors();
	std::printf("module %s\n", options.path.c_str());
	std::printf("  imports: %zu\n", imports.size());
	for (const WASM::ImportDescriptor& import : imports) {
		std::printf("    %s.%s: %s\n", import.moduleName.c_str(),
				import.fieldName.c_str(), kindName(import.type.kind));
	}
	std::printf("  exports: %zu\n", exports.size());
	for (const WASM::ExportDescriptor& descriptor : exports) {
		if (descriptor.type.kind == WASM::ExternalKind::Function) {
			std::string sig = "?";
			if (const WASM::Export* entry = findExport(module, descriptor.name)) {
				if (const auto typeIndex = functionTypeIndex(module, entry->index))
					sig = signature(module.funcTypeAt(*typeIndex));
			}
			std::printf("    %s: func %s\n", descriptor.name.c_str(), sig.c_str());
		} else {
			std::printf("    %s: %s\n", descriptor.name.c_str(),
					kindName(descriptor.type.kind));
		}
	}
	std::printf("  functions: %zu (%zu imported), memories: %zu, tables: %zu, "
				"globals: %zu, tags: %zu\n",
			module.importFunctions.size() + module.internalFunctionTypeIndices.size(),
			module.importFunctions.size(),
			module.importMemories.size() + module.memories.size(),
			module.importTables.size() + module.tables.size(),
			module.importGlobals.size() + module.globals.size(),
			module.importTags.size() + module.tags.size());
	std::fflush(stdout);

	// Instantiate exactly like a test: only the diagnostics log imports are
	// pre-registered. A module with other imports fails with the resolver's
	// named error and the resulting diagnostic.
	WASM::RegistryImportResolver registrations;
	WASM::registerToEnv(registrations);

	std::unique_ptr<TestBackend::Session> backend;
	std::unique_ptr<WASM::ModuleInstance> instance;
	try {
		backend = TestBackend::create();
		instance = backend->instantiator().instantiate(module, registrations);
	} catch (const std::exception& error) {
		return failWith("instantiation failed", error);
	}
	if (!instance) {
		std::fprintf(stderr, "diagnose: instantiation returned no instance\n");
		printRecentDiagnostics();
		return 1;
	}
	std::printf("  instantiated\n");
	std::fflush(stdout);

	if (options.invoke.empty())
		return 0;
	return invokeExport(module, *instance, *backend, options);
}

