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

// Forward declarations: the typed forms are defined next to the JSON ones, and
// the JSON ones call them.
std::string javaType(const ValueType& type);
std::string csharpType(const ValueType& type);

//! The `void` value a Dart signature names when a call returns nothing.
ValueType voidValue()
{
	ValueType type;
	type.kind = ValueKind::Void;
	return type;
}

std::string emitDartImpl(const Json& schema, const Schema& typed)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "// Generated from the " << typed.id << " "
		<< directionName(typed.direction) << " schema. Do not edit.\n\n";
	out << "import 'dart:ffi';\n\n";
	out << "typedef WasmV128 = Pointer<Uint8>;\n";
	for (const std::string& ref : refs)
		out << "typedef " << pascalName(ref) << " = Pointer<Void>;\n";
	out << "\n";

	const auto emitNative = [&](const Entry& entry,
								const std::string& functionName) {
		const std::vector<ValueType>& params = entry.function.params;
		const std::vector<ValueType>& results = entry.function.results;
		out << "@Native<"
			<< dartNativeType(results.size() == 1 ? results[0] : voidValue())
			<< " Function(";
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dartNativeType(params[i]);
		}
		out << ")>(" << "symbol: '" << functionName << "')\n";
		out << "external "
			<< (results.size() == 1 ? dartType(results[0]) : std::string("void"))
			<< " " << functionName << "(";
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dartType(params[i]) << " " << argumentName(entry, i);
		}
		out << ");\n\n";
	};

	forEachFunction(typed, false, [&](const Entry& entry) {
		out << "// H2G export: " << exportComment(entry) << "\n";
		emitNative(entry, "wasmjit_" + sanitize(entry.name));
	});
	forEachFunction(typed, true, [&](const Entry& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		emitNative(entry, importName(entry));
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string javaType(const Json& type)
{
	return javaType(ValueType::fromJson(type));
}

std::string javaType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "int";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "long";
	case ValueKind::F32: return "float";
	case ValueKind::F64: return "double";
	case ValueKind::V128: return "byte[]";
	case ValueKind::Void: return "void";
	default: break;
	}
	return "WasmExternref";
}

void emitJavaParams(std::ostringstream& out, const Entry& entry)
{
	const std::vector<ValueType>& params = entry.function.params;
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (i != 0) out << ", ";
		out << javaType(params[i]) << " " << argumentName(entry, i);
	}
	if (entry.function.results.size() > 1u) {
		if (!params.empty()) out << ", ";
		out << "long[] result";
	}
}

std::string emitJavaImpl(const Json& schema, const Schema& typed)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);
	std::ostringstream out;
	out << "// Generated Java/Wasm bindings from the " << typed.id << " "
		<< directionName(typed.direction) << " schema. Do not edit.\n";
	out << "package wasmjit.bindings;\n\n";
	out << "import org.teavm.interop.Export;\n";
	out << "import org.teavm.interop.Import;\n\n";
	out << "public final class WasmJitBindings {\n";
	out << "\tprivate WasmJitBindings() {}\n";
	for (const std::string& ref : refs)
		out << "\tpublic static final class " << pascalName(ref) << " { private "
			<< pascalName(ref) << "() {} }\n";
	out << "\n";
	forEachFunction(typed, false, [&](const Entry& entry) {
		const std::string& name = entry.name;
		out << "\t// H2G export: " << exportComment(entry) << "\n";
		out << "\t@Export(name = \"" << name << "\")\n";
		out << "\tpublic static "
			<< (entry.function.results.size() == 1
					? javaType(entry.function.results[0])
					: std::string("void"))
			<< " wasmjit_" << sanitize(name) << "(";
		emitJavaParams(out, entry);
		out << ")\n\t{\n\t\tthrow new UnsupportedOperationException(\"" << name
			<< " is not implemented\");\n\t}\n\n";
	});
	forEachFunction(typed, true, [&](const Entry& entry) {
		out << "\t// G2H import: " << importComment(entry) << "\n";
		out << "\t@Import(module = \"" << importModule(entry) << "\", name = \""
			<< entry.name << "\")\n";
		out << "\tprivate static native "
			<< (entry.function.results.size() == 1
					? javaType(entry.function.results[0])
					: std::string("void"))
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.name) << "(";
		emitJavaParams(out, entry);
		out << ");\n\n";
	});
	out << nonFunctionNotes(schema);
	out << "}\n";
	return out.str();
}

std::string csharpType(const Json& type)
{
	return csharpType(ValueType::fromJson(type));
}

std::string csharpType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "int";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "long";
	case ValueKind::F32: return "float";
	case ValueKind::F64: return "double";
	case ValueKind::V128: return "byte*";
	case ValueKind::Void: return "void";
	default: break;
	}
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
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "\tpublic struct " << pascalName(type) << " { }\n";
	out << "\n";
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
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "type " << pascalName(type) << " = nativeint\n";
	out << "\n";
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
