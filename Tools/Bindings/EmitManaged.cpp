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

std::string emitDartImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";
	out << "import 'dart:ffi';\n\n";
	out << "typedef WasmV128 = Pointer<Uint8>;\n";
	for (const std::string& ref : refs)
		out << "typedef " << pascalName(ref) << " = Pointer<Void>;\n";
	out << "\n";

	const auto emitNative = [&](const Json& entry, const std::string& functionName) {
		out << "@Native<" << dartNativeType(signatureResults(entry).size() == 1
												   ? signatureResults(entry)[0]
												   : Json{{"type", "void"}})
			<< " Function(";
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (i != 0) out << ", ";
			out << dartNativeType(signatureParams(entry)[i]);
		}
		out << ")>(" << "symbol: '" << functionName << "')\n";
		out << "external "
			<< (signatureResults(entry).size() == 1 ? dartType(signatureResults(entry)[0]) : "void")
			<< " " << functionName << "(";
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (i != 0) out << ", ";
			out << dartType(signatureParams(entry)[i]) << " " << argumentName(entry, i);
		}
		out << ");\n\n";
	};

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "// H2G export: " << exportComment(entry) << "\n";
		emitNative(entry,
				"wasmjit_" + sanitize(entry.at("name").get<std::string>()));
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		emitNative(entry, importName(entry));
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string javaType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "long";
	if (kind == "f32" || kind == "float32") return "float";
	if (kind == "f64" || kind == "float64") return "double";
	if (kind == "v128") return "byte[]";
	if (kind == "void") return "void";
	return "WasmExternref";
}

void emitJavaParams(std::ostringstream& out, const Json& entry)
{
	const Json& params = signatureParams(entry);
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (i != 0) out << ", ";
		out << javaType(params[i]) << " " << argumentName(entry, i);
	}
	if (signatureResults(entry).size() > 1u) {
		if (!params.empty()) out << ", ";
		out << "long[] result";
	}
}

std::string emitJavaImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);
	std::ostringstream out;
	out << "// Generated Java/Wasm bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "package wasmjit.bindings;\n\n";
	out << "import org.teavm.interop.Export;\n";
	out << "import org.teavm.interop.Import;\n\n";
	out << "public final class WasmJitBindings {\n";
	out << "\tprivate WasmJitBindings() {}\n";
	for (const std::string& ref : refs)
		out << "\tpublic static final class " << pascalName(ref)
			<< " { private " << pascalName(ref) << "() {} }\n";
	out << "\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = entry.at("name").get<std::string>();
		out << "\t// H2G export: " << exportComment(entry) << "\n";
		out << "\t@Export(name = \"" << name << "\")\n";
		out << "\tpublic static "
			<< (signatureResults(entry).size() == 1
					? javaType(signatureResults(entry)[0]) : "void")
			<< " wasmjit_" << sanitize(name) << "(";
		emitJavaParams(out, entry);
		out << ")\n\t{\n\t\tthrow new UnsupportedOperationException(\"" << name
			<< " is not implemented\");\n\t}\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "\t// G2H import: " << importComment(entry) << "\n";
		out << "\t@Import(module = \"" << importModule(entry) << "\", name = \""
			<< entry.at("name").get<std::string>() << "\")\n";
		out << "\tprivate static native "
			<< (signatureResults(entry).size() == 1
					? javaType(signatureResults(entry)[0]) : "void")
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		emitJavaParams(out, entry);
		out << ");\n\n";
	});
	out << nonFunctionNotes(schema);
	out << "}\n";
	return out.str();
}


std::string csharpType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "long";
	if (kind == "f32" || kind == "float32") return "float";
	if (kind == "f64" || kind == "float64") return "double";
	if (kind == "v128") return "byte*";
	if (kind == "void") return "void";
	return "void*";
}

void emitCSharpParams(std::ostringstream& out, const Json& entry)
{
	const Json& params = signatureParams(entry);
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (i != 0) out << ", ";
		out << csharpType(params[i]) << " " << argumentName(entry, i);
	}
	if (signatureResults(entry).size() > 1u) {
		if (!params.empty()) out << ", ";
		out << "byte* result";
	}
}

std::string emitCSharpImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated C#/.NET Wasm bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "using System;\nusing System.Runtime.InteropServices;\n\n";
	out << "public static unsafe class WasmJitBindings\n{\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = entry.at("name").get<std::string>();
		out << "\t// H2G export: " << exportComment(entry) << "\n";
		out << "\t[UnmanagedCallersOnly(EntryPoint = \"" << name << "\")]\n";
		out << "\tpublic static "
			<< (signatureResults(entry).size() == 1
					? csharpType(signatureResults(entry)[0]) : "void")
			<< " wasmjit_" << sanitize(name) << "(";
		emitCSharpParams(out, entry);
		out << ")\n\t{\n\t\tthrow new NotSupportedException(\"" << name
			<< " is not implemented\");\n\t}\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "\t// G2H import: " << importComment(entry) << "\n";
		out << "\t[DllImport(\"" << importModule(entry) << "\", EntryPoint = \""
			<< entry.at("name").get<std::string>()
			<< "\", CallingConvention = CallingConvention.Cdecl)]\n";
		out << "\tpublic static extern "
			<< (signatureResults(entry).size() == 1
					? csharpType(signatureResults(entry)[0]) : "void")
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		emitCSharpParams(out, entry);
		out << ");\n\n";
	});
	out << nonFunctionNotes(schema);
	out << "}\n";
	return out.str();
}


std::string emitFSharpImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated F#/.NET Wasm bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "open System\nopen System.Runtime.InteropServices\n\n";
	out << "module WasmJitBindings\n\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = entry.at("name").get<std::string>();
		out << "// H2G export: " << exportComment(entry) << "\n";
		out << "[<UnmanagedCallersOnly(EntryPoint = \"" << name << "\")>]\n";
		out << "let wasmjit_" << sanitize(name) << " ";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i)
			out << "(" << argumentName(entry, i) << " : " << csharpType(params[i]) << ") ";
		out << ": "
			<< (signatureResults(entry).size() == 1
					? csharpType(signatureResults(entry)[0]) : "unit")
			<< " =\n\traise (NotSupportedException(\"" << name
			<< " is not implemented\"))\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << "[<DllImport(\"" << importModule(entry) << "\", EntryPoint = \""
			<< entry.at("name").get<std::string>()
			<< "\", CallingConvention = CallingConvention.Cdecl)>]\n";
		out << "extern " << (signatureResults(entry).size() == 1
				? csharpType(signatureResults(entry)[0]) : "void")
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << " : " << csharpType(params[i]);
		}
		out << ")\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}




} // namespace WASM::Bindings::detail
