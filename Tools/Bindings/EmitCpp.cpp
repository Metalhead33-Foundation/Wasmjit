#include "EmitInternal.hpp"
#include "EmitShared.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

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
	out << "namespace WASM { struct VMContext; struct ModuleInstance; class ImportRegistrar; }\n";
	out << "struct wasm_v128 { std::uint8_t bytes[16]; };\n";
	for (const std::string& ref : refs)
		out << "struct " << ref << ";\n";
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
			<< "_register_imports(WASM::ImportRegistrar& registrar);\n";
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
		out << "#include <WasmBase/WasmImport.hpp>\n"
			   "#include <WasmBase/WasmNativeImport.hpp>\n";
	if (emitExports)
		out << "#include <WasmBase/WasmModuleInstance.hpp>\n";
	out << "\n";
	if (emitImports) {
		forEachFunction(schema, "imports", [&](const Json& entry) {
			emitCppImportStub(out, entry);
		});
		out << "// Publish the host implementations as a synthetic Wasm import module.\n";
		out << "// This is C++, not part of the C ABI surface.\n";
		out << "bool wasmjit_" << module
			<< "_register_imports(WASM::ImportRegistrar& registrar)\n{\n";
		forEachFunction(schema, "imports", [&](const Json& entry) {
			out << "\tregistrar.registerFunction(\"" << importModule(entry)
				<< "\", \"" << entry.at("name").get<std::string>() << "\",\n"
				<< "\t\t\tWASM::nativeImport<&" << importName(entry) << ">());\n";
		});
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
		forEachFunction(schema, "exports", [&](const Json& entry) {
			const std::string name = entry.at("name").get<std::string>();
			out << "\t{\n";
			out << "\t\tconst auto callable = instance->exportedFunction(\""
				<< name << "\");\n";
			out << "\t\tif (!callable) {\n";
			out << "\t\t\t" << cppCallableName(entry) << " = nullptr;\n";
			out << "\t\t\tok = false;\n";
			out << "\t\t} else {\n";
			out << "\t\t\t" << cppCallableName(entry) << " = reinterpret_cast<"
				<< cppTypedefName(entry) << ">(callable->fnPtr);\n";
			out << "\t\t}\n\t}\n";
		});
		out << "\treturn ok;\n}\n";
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
