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

std::string emitTypeScriptImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "/* Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit. */\n\n";
	out << "export type wasm_v128 = Uint8Array;\n";
	for (const std::string& ref : refs)
		out << "export interface " << pascalName(ref) << " { readonly __" << ref
			<< ": unique symbol; }\n";
	out << "\n// Reference types are opaque. Reference parameters are borrowed unless\n"
		   "// the adapter says otherwise; reference results carry an explicit\n"
		   "// ownership note.\n\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "// H2G export: " << exportComment(entry) << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << tsType(params[i]);
			if (isReference(params[i])) out << " /* borrowed */";
		}
		if (params.empty()) out << "void";
		out << ")";
		const Json& results = signatureResults(entry);
		if (results.size() == 1) {
			out << ": " << tsType(results[0]);
			if (isReference(results[0])) out << " /* ownership: explicit */";
		} else if (results.size() > 1) {
			out << ": [";
			for (std::size_t i = 0; i < results.size(); ++i) {
				if (i != 0) out << ", ";
				out << tsType(results[i]);
			}
			out << "]";
		} else {
			out << ": void";
		}
		out << ";\n\n";
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << tsType(params[i]);
			if (isReference(params[i])) out << " /* borrowed */";
		}
		if (params.empty()) out << "void";
		out << ")";
		const Json& results = signatureResults(entry);
		if (results.size() == 1)
			out << ": " << tsType(results[0]);
		else if (results.size() > 1)
			out << ": void";
		else
			out << ": void";
		out << ";\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string emitAssemblyScriptImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";

	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "@external(\"" << importModule(entry) << "\", \""
			<< entry.at("name").get<std::string>() << "\")\n";
		out << "export declare function wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << asType(params[i]);
		}
		out << ")";
		const Json& results = signatureResults(entry);
		out << (results.size() == 1 ? ": " + asType(results[0]) : ": void");
		out << ";\n\n";
	});

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "// H2G export: " << exportComment(entry) << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << asType(params[i]);
		}
		out << ")";
		const Json& results = signatureResults(entry);
		out << (results.size() == 1 ? ": " + asType(results[0]) : ": void");
		out << ";\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string emitZigImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";
	out << "pub const wasm_v128 = [16]u8;\n";
	for (const std::string& ref : refs)
		out << "pub const " << ref << " = opaque {};\n";
	out << "\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string exportName = entry.at("name").get<std::string>();
		out << "// H2G export: " << exportComment(entry) << "\n";
		out << "pub const wasmjit_" << sanitize(exportName) << "_t = *const fn (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << zigType(params[i]);
		}
		const bool multi = signatureResults(entry).size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result: *anyopaque";
		}
		out << ") callconv(.c) ";
		if (multi || signatureResults(entry).empty())
			out << "void";
		else
			out << zigType(signatureResults(entry)[0]);
		out << ";\n\n";

		// A type alias is useful to a host loader, but a Zig guest needs the
		// actual exported definition. Keep it compilable with a trap body.
		out << "pub fn wasmjit_" << sanitize(exportName) << "_impl(";
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << zigType(params[i]);
		}
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result: *anyopaque";
		}
		out << ") callconv(.c) ";
		if (multi || signatureResults(entry).empty())
			out << "void";
		else
			out << zigType(signatureResults(entry)[0]);
		out << " {\n";
		for (std::size_t i = 0; i < params.size(); ++i)
			out << "    _ = " << argumentName(entry, i) << ";\n";
		if (multi)
			out << "    _ = result;\n";
		out << "    @panic(" << quoteName("implement " + exportName, false)
			<< ");\n}\n";
		out << "comptime {\n    @export(&wasmjit_" << sanitize(exportName)
			<< "_impl, .{ .name = " << quoteName(exportName, false)
			<< " });\n}\n\n";
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		// `@extern` keeps the local Zig name namespaced while mapping the
		// declaration to the actual Wasm import module and field.
		out << "pub const wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>())
			<< " = @extern(*const fn (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << zigType(params[i]);
		}
		const bool multi = signatureResults(entry).size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result: *anyopaque";
		}
		out << ") callconv(.c) ";
		if (multi || signatureResults(entry).empty())
			out << "void";
		else
			out << zigType(signatureResults(entry)[0]);
		out << ", .{ .name = " << quoteName(entry.at("name").get<std::string>(), false)
			<< ", .library_name = " << quoteName(importModule(entry), false)
			<< " });\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string emitKotlinImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";
	out << "class WasmV128(val bytes: ByteArray)\n";
	for (const std::string& ref : refs)
		out << "class " << pascalName(ref) << " private constructor()\n";
	out << "\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "// H2G export: " << exportComment(entry) << "\n";
		out << "typealias wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_t = (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << kotlinType(params[i]);
		}
		out << ") -> "
			<< (signatureResults(entry).size() == 1 ? kotlinType(signatureResults(entry)[0])
											   : "Unit");
		out << "\n\n";
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << "external fun wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << kotlinType(params[i]);
		}
		out << "): "
			<< (signatureResults(entry).size() == 1 ? kotlinType(signatureResults(entry)[0])
											   : "Unit");
		out << "\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string emitMoonBitImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";
	for (const std::string& ref : refs)
		out << "#external\ntype " << pascalName(ref) << "\n";
	out << "\n";

	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		std::string borrow;
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (isReference(signatureParams(entry)[i])) {
				if (!borrow.empty()) borrow += ", ";
				borrow += argumentName(entry, i);
			}
		}
		if (!borrow.empty())
			out << "#borrow(" << borrow << ")\n";
		out << "extern \"wasm\" fn wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << " : " << moonbitType(signatureParams(entry)[i]);
		}
		out << ") -> "
			<< (signatureResults(entry).size() == 1 ? moonbitType(signatureResults(entry)[0])
											   : "Unit");
		out << " = \"" << importModule(entry) << "\" \""
			<< entry.at("name").get<std::string>() << "\"\n\n";
	});

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "// H2G export: " << exportComment(entry) << "(";
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (i != 0) out << ", ";
			out << moonbitType(signatureParams(entry)[i]);
		}
		out << ") -> "
			<< (signatureResults(entry).size() == 1 ? moonbitType(signatureResults(entry)[0])
											   : "Unit")
			<< "\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string emitGoImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "package wasmjitbindings\n\n";
	for (const std::string& ref : refs)
		out << "type " << pascalName(ref) << " struct{ _ [0]byte }\n";
	out << "\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "//go:wasmexport " << entry.at("name").get<std::string>() << "\n";
		out << "func wasmjit_" << sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << " " << goType(params[i]);
		}
		const bool multi = signatureResults(entry).size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result *byte";
		}
		out << ")";
		if (!multi && signatureResults(entry).size() == 1)
			out << " " << goType(signatureResults(entry)[0]);
		out << " { panic(\"generated adapter stub\") }\n\n";
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "//go:wasmimport " << importModule(entry) << " "
			<< entry.at("name").get<std::string>() << "\n";
		out << "func wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << " " << goType(params[i]);
		}
		const bool multi = signatureResults(entry).size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result *byte";
		}
		out << ")";
		if (!multi && signatureResults(entry).size() == 1)
			out << " " << goType(signatureResults(entry)[0]);
		out << "\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string dartNativeType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s32") return "Int32";
	if (kind == "u32") return "Uint32";
	if (kind == "i64" || kind == "s64") return "Int64";
	if (kind == "u64") return "Uint64";
	if (kind == "s8") return "Int8";
	if (kind == "u8") return "Uint8";
	if (kind == "s16") return "Int16";
	if (kind == "u16") return "Uint16";
	if (kind == "f32" || kind == "float32") return "Float";
	if (kind == "f64" || kind == "float64") return "Double";
	if (kind == "bool") return "Bool";
	if (kind == "char") return "Uint32";
	if (kind == "string") return "Pointer<Utf8>";
	if (kind == "v128") return "Pointer<Uint8>";
	if (kind == "void") return "Void";
	if (kind == "error-context" || isReference(type)) return "Pointer<Void>";
	return "Pointer<Void>";
}


} // namespace WASM::Bindings::detail
