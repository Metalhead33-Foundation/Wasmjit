#include "EmitInternal.hpp"
#include "EmitShared.hpp"
#include "frontend/wit/WitFrontend.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace WASM::Bindings::detail {

using Json = nlohmann::ordered_json;

std::string cppTypedefName(const Json &entry) {
	return "wasmjit_" + sanitize(entry.at("name").get<std::string>()) + "_fn";
}

std::string cppCallableName(const Json &entry) {
	return "wasmjit_" + sanitize(entry.at("name").get<std::string>());
}

std::string cppReturnType(const Json &entry) {
	const Json &results = signatureResults(entry);
	return results.size() == 1 ? cType(results[0]) : "void";
}

void emitCppParams(std::ostringstream &out, const Json &entry) {
	const Json &params = signatureParams(entry);
	out << "WASM::VMContext* context";
	for (std::size_t i = 0; i < params.size(); ++i)
		out << ", " << cType(params[i]) << " " << argumentName(entry, i);
	if (signatureResults(entry).size() > 1u)
		out << ", void* result";
}

void emitCppPointerTypedef(std::ostringstream &out, const Json &entry,
			   const std::string &typedefName) {
	out << "using " << typedefName << " = " << cppReturnType(entry) << " (*)(";
	emitCppParams(out, entry);
	out << ");";
	if (signatureResults(entry).size() == 1 && isReference(signatureResults(entry)[0]))
		out << " // result ownership: explicit";
	out << "\n";
}

void emitCppImportDeclaration(std::ostringstream &out, const Json &entry) {
	// A G2H import is a function the host implements; no function-pointer
	// typedef is needed for the declaration.
	out << cppReturnType(entry) << " " << importName(entry) << "(";
	emitCppParams(out, entry);
	out << ");\n";
}

void emitCppExportDeclaration(std::ostringstream &out, const Json &entry) {
	emitCppPointerTypedef(out, entry, cppTypedefName(entry));
	out << "extern " << cppTypedefName(entry) << " " << cppCallableName(entry) << ";\n";
}

void emitCppImportStub(std::ostringstream &out, const Json &entry) {
	const Json &results = signatureResults(entry);
	const std::string name = importName(entry);
	out << "// host import: " << importComment(entry) << "\n";
	out << cppReturnType(entry) << " " << name << "(";
	emitCppParams(out, entry);
	out << ")\n{\n";
	out << "\t(void)context;\n";
	for (std::size_t i = 0; i < signatureParams(entry).size(); ++i)
		out << "\t(void)" << argumentName(entry, i) << ";\n";
	if (results.size() > 1u)
		out << "\t(void)result;\n";
	out << "\t// TODO: implement this host import.\n";
	if (results.size() == 1 && isReference(results[0]))
		out << "\treturn nullptr;\n";
	else if (results.size() == 1)
		out << "\treturn {};\n";
	out << "}\n\n";
}

std::string witCppDefinitionName(const Json& document, std::size_t index)
{
	return witOpaqueTypeName(witTypeDisplayName(document, index));
}

std::string witCppType(const Json& document, const Json& type)
{
	return cType(witResolveType(document, type));
}

bool witCppIsVoid(const Json& document, const Json& type)
{
	if (type.is_null())
		return true;
	const Json resolved = witResolveType(document, type);
	return resolved.value("type", "") == "void";
}

void emitWitCppNamedField(std::ostringstream& out, const Json& document,
		const Json& field)
{
	if (!field.is_object() || !field.contains("name")
			|| !field.at("name").is_string())
		return;
	out << "\t" << witCppType(document, field.value("type", Json{})) << " "
		<< sanitize(field.at("name").get<std::string>()) << ";\n";
}

void emitWitCppRecord(std::ostringstream& out, const Json& document,
		const Json& kind)
{
	const auto fields = kind.at("record").find("fields");
	if (fields == kind.at("record").end() || !fields->is_array())
		return;
	for (const Json& field : *fields)
		emitWitCppNamedField(out, document, field);
}

void emitWitCppVariant(std::ostringstream& out, const Json& document,
		const Json& kind)
{
	const Json& variant = kind.at("variant");
	const auto cases = variant.find("cases");
	if (cases == variant.end() || !cases->is_array())
		return;
	out << "\tenum class Tag : std::uint32_t {\n";
	std::size_t caseIndex = 0;
	for (const Json& alternative : *cases) {
		out << "\t\t" << sanitize(alternative.value("name", std::string()))
			<< " = " << caseIndex++ << ",\n";
	}
	out << "\t};\n\tTag tag;\n";
	bool hasPayload = false;
	for (const Json& alternative : *cases)
		if (alternative.contains("type") && !alternative.at("type").is_null())
			hasPayload = true;
	if (!hasPayload)
		return;
	out << "\tunion {\n";
	for (const Json& alternative : *cases) {
		if (!alternative.contains("type") || alternative.at("type").is_null())
			continue;
		out << "\t\t" << witCppType(document, alternative.at("type")) << " "
			<< sanitize(alternative.value("name", std::string())) << ";\n";
	}
	out << "\t} payload;\n";
}

void emitWitCppTuple(std::ostringstream& out, const Json& document,
		const Json& kind)
{
	const auto types = kind.at("tuple").find("types");
	if (types == kind.at("tuple").end() || !types->is_array())
		return;
	std::size_t fieldIndex = 0;
	for (const Json& field : *types)
		out << "\t" << witCppType(document, field) << " field_" << fieldIndex++
			<< ";\n";
}

void emitWitCppFlags(std::ostringstream& out, const Json& kind,
		const std::string& name)
{
	const Json& flags = kind.at("flags");
	const auto values = flags.find("flags");
	if (values == flags.end() || !values->is_array())
		return;
	std::size_t bit = 0;
	for (const Json& flag : *values) {
		out << "constexpr " << name << " "
			<< sanitize(name + "_" + flag.value("name", std::string())) << " = "
			<< "(" << name << ")(1u << " << bit++ << ");\n";
	}
}

void emitWitCppTypeDefinitions(std::ostringstream& out, const Json& schema)
{
	if (schema.value("frontend", "") != "wit"
			|| !schema.contains("types") || !schema.at("types").is_array())
		return;
	const Json document = { { "types", schema.at("types") } };
	std::set<std::string> emitted;
	std::size_t index = 0;
	for (const Json& definition : schema.at("types")) {
		++index;
		if (!definition.is_object() || witTypeIsAlias(definition)
				|| !definition.contains("kind"))
			continue;
		const Json& kind = definition.at("kind");
		const std::string name = witCppDefinitionName(document, index - 1);
		if (!emitted.insert(name).second)
			continue;
		if (kind.is_string()) {
			if (kind.get<std::string>() == "resource") {
				out << "// WIT resource handle; ownership is tracked by the\n"
					   "// backend resource table.\n"
					<< "struct " << name << " {\n\tuint32_t handle;\n};\n";
			}
			continue;
		}
		if (!kind.is_object())
			continue;
		if (kind.contains("enum")) {
			const auto cases = kind.at("enum").find("cases");
			out << "enum class " << name << " : std::uint32_t {\n";
			if (cases != kind.at("enum").end() && cases->is_array()) {
				std::size_t value = 0;
				for (const Json& entry : *cases) {
					const std::string caseName = entry.is_string()
							? entry.get<std::string>()
							: entry.value("name", std::string());
					out << "\t" << sanitize(caseName) << " = " << value++
						<< ",\n";
				}
			}
			out << "};\n";
		} else if (kind.contains("flags")) {
			emitWitCppFlags(out, kind, name);
		} else if (kind.contains("record") || kind.contains("variant")
				|| kind.contains("tuple") || kind.contains("list")
				|| kind.contains("option") || kind.contains("result")
				|| kind.contains("handle") || kind.contains("stream")
				|| kind.contains("future")) {
			if (kind.contains("handle")) {
				const Json& handle = kind.at("handle");
				out << (handle.contains("own")
						? "// owned WIT handle; release through the backend resource table.\n"
						: "// borrowed WIT handle; valid for the enclosing call scope.\n");
			}
			out << "struct " << name << " {\n";
			if (kind.contains("record"))
				emitWitCppRecord(out, document, kind);
			else if (kind.contains("variant"))
				emitWitCppVariant(out, document, kind);
			else if (kind.contains("tuple"))
				emitWitCppTuple(out, document, kind);
			else if (kind.contains("list")) {
				out << "\tuint32_t length;\n\t"
					<< witCppType(document, kind.at("list")) << "* data;\n";
			} else if (kind.contains("option")) {
				out << "\tuint8_t is_some;\n";
				if (!witCppIsVoid(document, kind.at("option")))
					out << "\t" << witCppType(document, kind.at("option"))
						<< " value;\n";
			} else if (kind.contains("result")) {
				out << "\tuint8_t is_err;\n";
				if (!witCppIsVoid(document, kind.at("result").value("ok", Json{})))
					out << "\t" << witCppType(document,
							kind.at("result").value("ok", Json{})) << " ok;\n";
				if (!witCppIsVoid(document, kind.at("result").value("err", Json{})))
					out << "\t" << witCppType(document,
							kind.at("result").value("err", Json{})) << " err;\n";
			} else if (kind.contains("handle"))
				out << "\tuint32_t handle;\n";
			else if (kind.contains("stream") || kind.contains("future"))
				out << "// stream/future endpoint; drop both ends through the backend.\n"
					<< "\tuint32_t handle;\n";
			out << "};\n";
		}
	}
}

std::string emitCppTypedefsImpl(const Json& schema)
{
	const std::string direction = schema.value("direction", "g2h");
	if (direction == "g2g")
		return "// cpp-typedefs is not applicable to G2G; project the producer "
			   "schema to g2h or h2g first.\n";

	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);
	const bool emitImports = direction != "h2g";
	const bool emitExports = direction != "g2h";
	std::ostringstream out;
	out << "// Generated C++ binding surface from the "
		<< schema.at("schema").get<std::string>() << " " << direction
		<< " schema. Do not edit.\n";
	out << "#ifndef WASMJIT_GENERATED_" << sanitize(schema.value("module", "module"))
		<< "_CALLABLES_HPP\n#define WASMJIT_GENERATED_"
		<< sanitize(schema.value("module", "module")) << "_CALLABLES_HPP\n\n";
	out << "#include <cstdint>\n\n";
	out << "namespace WASM { struct VMContext; struct ModuleInstance; class RegistryImportResolver; }\n";
	out << "struct wasm_v128 { std::uint8_t bytes[16]; };\n";
	emitWitOpaqueTypes(out, schema, true);
	emitWitCppTypeDefinitions(out, schema);
	for (const std::string& ref : refs) {
		if (ref.rfind("wasmjit_wit_", 0) == 0)
			continue;
		out << "struct " << ref << ";\n";
	}
	out << "\n// A Callable's fnPtr is `ret f(VMContext*, params...)`; multi-value\n"
		   "// results use a trailing result buffer.\n";
	if (emitImports) {
		out << "\nextern \"C\" {\n";
		out << "\n// Host imports the guest expects you to implement (G2H).\n";
		forEachFunction(schema, "imports", [&](const Json& entry) {
			emitCppImportDeclaration(out, entry);
		});
		out << "\n} // extern \"C\"\n";
		out << "\n// C++-only: publish the implementations above into a synthetic\n"
			   "// Wasm import module. This declaration is deliberately outside\n"
			   "// the C ABI block.\n";
		out << "bool wasmjit_" << sanitize(schema.value("module", "module"))
			<< "_register_imports(WASM::RegistryImportResolver& registrar);\n";
	}
	if (emitExports) {
		out << "\n// Guest exports the host can resolve (H2G).\n";
		forEachFunction(schema, "exports", [&](const Json& entry) {
			out << "// export: " << exportComment(entry) << "\n";
			emitCppExportDeclaration(out, entry);
		});
		out << "\n// C++-only: resolve the pointers above from a loaded module instance.\n";
		out << "bool wasmjit_" << sanitize(schema.value("module", "module"))
			<< "_load_callables(WASM::ModuleInstance* instance);\n";
	}
	out << nonFunctionNotes(schema);
	out << "\n#endif // WASMJIT_GENERATED_"
		<< sanitize(schema.value("module", "module")) << "_CALLABLES_HPP\n";
	return out.str();
}


std::string emitCppSourceImpl(const Json& schema)
{
	const std::string direction = schema.value("direction", "g2h");
	if (direction == "g2g")
		return "// cpp-source is not applicable to G2G; project the producer "
			   "schema to g2h or h2g first.\n";

	const bool emitImports = direction != "h2g";
	const bool emitExports = direction != "g2h";
	const std::string module = sanitize(schema.value("module", "module"));
	std::ostringstream out;
	out << "// Generated C++ binding implementation from the "
		<< schema.at("schema").get<std::string>() << " " << direction
		<< " schema. Do not edit.\n";
	out << "#include \"wasmjit_" << module << "_callables.hpp\"\n";
	if (emitImports)
		out << "#include <WasmBase/WasmRegistryImportResolver.hpp>\n"
			   "#include <WasmBase/WasmNativeImport.hpp>\n";
	if (emitExports)
		out << "#include <WasmBase/WasmModuleInstance.hpp>\n"
			   "#include <array>\n"
			   "#include <optional>\n"
			   "#include <string_view>\n"
			   "#include <type_traits>\n";
	out << "\n";
	if (emitImports) {
		forEachFunction(schema, "imports", [&](const Json& entry) {
			emitCppImportStub(out, entry);
		});
		out << "// Publish the host implementations as a synthetic Wasm import module.\n";
		out << "// This is C++, not part of the C ABI surface; the concrete module\n";
		out << "// registry avoids one virtual module lookup per function.\n";
		out << "bool wasmjit_" << module
			<< "_register_imports(WASM::RegistryImportResolver& registrar)\n{\n";
		std::map<std::string, std::vector<const Json*>> importsByModule;
		forEachFunction(schema, "imports", [&](const Json& entry) {
			importsByModule[importModule(entry)].push_back(&entry);
		});
		for (const auto& [importModuleName, entries] : importsByModule) {
			out << "\t{\n\t\tauto& module = registrar.moduleRegistry("
				<< quoteName(importModuleName, false) << ");\n";
			out << "\t\tmodule.registerFunctions({\n";
			for (const Json* entry : entries) {
				out << "\t\t\t{ " << quoteName(entry->at("name").get<std::string>(), false)
					<< ",\n\t\t\t  WASM::nativeImport<&" << importName(*entry)
					<< ">() },\n";
			}
			out << "\t\t});\n\t}\n";
		}
		out << "\treturn true;\n}\n\n";
	}
	if (emitExports) {
		out << "// Definitions of the resolveable guest export pointers.\n";
		forEachFunction(schema, "exports", [&](const Json& entry) {
			out << cppTypedefName(entry) << " " << cppCallableName(entry)
				<< " = nullptr;\n";
		});
		out << "\n// Resolve every guest export from a module instance.\n";
		out << "// C++ forbids static_cast from void* to a function pointer; the\n"
			   "// Callable ABI boundary uses reinterpret_cast.\n";
		out << "bool wasmjit_" << module
			<< "_load_callables(WASM::ModuleInstance* instance)\n{\n";
		out << "\tif (instance == nullptr)\n\t\treturn false;\n";
		out << "\tbool ok = true;\n";
		std::size_t exportCount = 0;
		forEachFunction(schema, "exports", [&](const Json&) { ++exportCount; });
		if (exportCount == 0) {
			out << "\treturn ok;\n}\n";
		} else {
			out << "\tstatic constexpr std::array<std::string_view, " << exportCount
				<< "> names = {\n";
			forEachFunction(schema, "exports", [&](const Json& entry) {
				out << "\t\t" << quoteName(entry.at("name").get<std::string>(), false)
					<< ",\n";
			});
			out << "\t};\n";
			out << "\tstd::array<std::optional<WASM::Callable>, " << exportCount
				<< "> callables{};\n";
			out << "\tinstance->exportedFunctions(names, callables);\n";
			out << "\tconst auto assignCallable = [&](const std::optional<WASM::Callable>& callable,\n"
				   "\t\t\t\t\t\t\t\t auto& target) {\n";
			out << "\t\tusing Target = std::decay_t<decltype(target)>;\n";
			out << "\t\tif (!callable) {\n";
			out << "\t\t\ttarget = nullptr;\n";
			out << "\t\t\tok = false;\n";
			out << "\t\t\treturn;\n";
			out << "\t\t}\n";
			out << "\t\ttarget = reinterpret_cast<Target>(callable->fnPtr);\n";
			out << "\t};\n";
			std::size_t index = 0;
			forEachFunction(schema, "exports", [&](const Json& entry) {
				out << "\tassignCallable(callables[" << index++ << "], "
					<< cppCallableName(entry) << ");\n";
			});
			out << "\treturn ok;\n}\n";
		}
	}
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string emitCppAdapterImpl(const Json& schema)
{
	const std::string direction = schema.value("direction", "g2h");
	if (direction != "h2g")
		return "// cpp-adapter is an H2G (host calls guest) surface; no output "
			   "for " + direction + ".\n";
	if (schema.value("kind", "module") == "component")
		return "// Component adapters are not generated yet; use the C header and "
			   "ComponentFunction.\n";

	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "// Generated adapter from the " << schema.at("schema").get<std::string>()
		<< " " << schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "#include <WasmBase/WasmRuntime.hpp>\n";
	out << "#include <cstdint>\n";
	out << "#include <stdexcept>\n";
	out << "#include <string>\n\n";
	out << "struct wasm_v128 { std::uint8_t bytes[16]; };\n";
	emitWitOpaqueTypes(out, schema, true);
	for (const std::string& ref : refs)
		out << "struct " << ref << ";\n";
	out << "\nWASM::ModuleInstance* wasmjit_adapter_instance = nullptr;\n\n";
	out << "namespace {\n";
	out << "template <typename Ret, typename... Args>\n";
	out << "Ret callExport(const char* name, Args... args)\n";
	out << "{\n";
	out << "\tif (wasmjit_adapter_instance == nullptr)\n";
	out << "\t\tthrow std::runtime_error(\"wasmjit adapter: no instance\");\n";
	out << "\tauto callable = wasmjit_adapter_instance->exportedFunction(name);\n";
	out << "\tif (!callable)\n";
	out << "\t\tthrow std::runtime_error(std::string(\"wasmjit adapter: missing export \") "
		   "+ name);\n";
	out << "\treturn WASM::callCallable<Ret>(*callable, args...);\n";
	out << "}\n\n";
	out << "template <typename... Args>\n";
	out << "void callExportVoid(const char* name, Args... args)\n";
	out << "{\n";
	out << "\tif (wasmjit_adapter_instance == nullptr)\n";
	out << "\t\tthrow std::runtime_error(\"wasmjit adapter: no instance\");\n";
	out << "\tauto callable = wasmjit_adapter_instance->exportedFunction(name);\n";
	out << "\tif (!callable)\n";
	out << "\t\tthrow std::runtime_error(std::string(\"wasmjit adapter: missing export \") "
		   "+ name);\n";
	out << "\tWASM::callCallable<void>(*callable, args...);\n";
	out << "}\n\n";
	out << "template <typename... Args>\n";
	out << "void callExportWithResults(const char* name, void* results, Args... args)\n";
	out << "{\n";
	out << "\tif (wasmjit_adapter_instance == nullptr)\n";
	out << "\t\tthrow std::runtime_error(\"wasmjit adapter: no instance\");\n";
	out << "\tauto callable = wasmjit_adapter_instance->exportedFunction(name);\n";
	out << "\tif (!callable)\n";
	out << "\t\tthrow std::runtime_error(std::string(\"wasmjit adapter: missing export \") "
		   "+ name);\n";
	out << "\tWASM::callCallableWithResults(*callable, results, args...);\n";
	out << "}\n";
	out << "} // namespace\n\n";
	out << "extern \"C\" {\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const Json& params = signatureParams(entry);
		const Json& results = signatureResults(entry);
		const std::string exportName = entry.at("name").get<std::string>();
		const std::string name = "wasmjit_" + sanitize(exportName);
		out << "\n// H2G export: " << exportComment(entry) << "\n";
		const bool multiValue = results.size() > 1u;
		const std::string ret = results.size() == 1 ? cType(results[0]) : "void";
		out << ret << " " << name << "(";
		if (params.empty() && !multiValue)
			out << "void";
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << cType(params[i]) << " " << argumentName(entry, i);
		}
		if (multiValue) {
			if (!params.empty()) out << ", ";
			out << "void* result";
		}
		out << ")\n{\n";
		std::string callArgs;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) callArgs += ", ";
			if (isReference(params[i]))
				callArgs += "reinterpret_cast<void*>(" + argumentName(entry, i) + ")";
			else
				callArgs += argumentName(entry, i);
		}
		if (multiValue) {
			out << "\tcallExportWithResults(\"" << exportName << "\", result";
			if (!callArgs.empty()) out << ", " << callArgs;
			out << ");\n";
		} else if (results.empty()) {
			out << "\tcallExportVoid(\"" << exportName << "\"";
			if (!callArgs.empty()) out << ", " << callArgs;
			out << ");\n";
		} else if (isReference(results[0])) {
			out << "\treturn reinterpret_cast<" << ret << ">(callExport<void*>(\""
				<< exportName << "\"";
			if (!callArgs.empty()) out << ", " << callArgs;
			out << "));\n";
		} else {
			out << "\treturn callExport<" << ret << ">(\"" << exportName << "\"";
			if (!callArgs.empty()) out << ", " << callArgs;
			out << ");\n";
		}
		out << "}\n";
	});
	out << "\n} // extern \"C\"\n";
	out << nonFunctionNotes(schema);
	return out.str();
}

} // namespace WASM::Bindings::detail
