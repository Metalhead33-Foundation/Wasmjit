#include "EmitShared.hpp"

#include "BindingModel.hpp"
#include "frontend/wit/WitFrontend.hpp"

#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace WASM::Bindings::detail {

void dedupeJsonSchema(Json& schema)
{
	const auto dedupe = [](Json& entries, bool imports) {
		if (!entries.is_array())
			return;
		std::set<std::string> seen;
		Json unique = Json::array();
		for (const Json& entry : entries) {
			if (entry.is_object() && entry.value("kind", "") == "function") {
				const std::string key = imports
						? entry.value("module", std::string()) + "\x1f"
								+ entry.value("name", std::string())
						: entry.value("name", std::string());
				if (!seen.insert(key).second)
					continue;
			}
			unique.push_back(entry);
		}
		entries = std::move(unique);
	};
	if (schema.contains("imports"))
		dedupe(schema["imports"], true);
	if (schema.contains("exports"))
		dedupe(schema["exports"], false);
}

std::string sanitize(std::string_view text)
{
	std::string out;
	for (const char c : text)
		out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
	if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())))
		out.insert(0, "x_");
	return out;
}

//! Quotes a schema name for a C or Rust string literal. Wasm import names are
//! arbitrary UTF-8, so they cannot be interpolated into an annotation raw.
std::string quoteName(std::string_view text, bool rust)
{
	std::string out = "\"";
	for (const char raw : text) {
		const unsigned char c = static_cast<unsigned char>(raw);
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20 || c == 0x7f) {
				static const char* const hex = "0123456789abcdef";
				out += rust ? "\\u{" : "\\x";
				if (rust) {
					out.push_back(hex[(c >> 4) & 0xf]);
					out.push_back(hex[c & 0xf]);
					out.push_back('}');
				} else {
					out.push_back(hex[(c >> 4) & 0xf]);
					out.push_back(hex[c & 0xf]);
				}
			} else {
				out.push_back(raw);
			}
			break;
		}
	}
	out.push_back('"');
	return out;
}

bool isReference(const Json& type)
{
	const ValueType value = ValueType::fromJson(type);
	return value.kind == ValueKind::Ref || value.kind == ValueKind::StringRef
			|| value.kind == ValueKind::StringViewWtf8
			|| value.kind == ValueKind::StringViewWtf16
			|| value.kind == ValueKind::StringViewIter;
}

std::string refTypeName(const Json& type)
{
	const ValueType value = ValueType::fromJson(type);
	switch (value.kind) {
	case ValueKind::StringRef: return "wasm_stringref";
	case ValueKind::StringViewWtf8: return "wasm_stringview_wtf8";
	case ValueKind::StringViewWtf16: return "wasm_stringview_wtf16";
	case ValueKind::StringViewIter: return "wasm_stringview_iter";
	case ValueKind::Ref: break;
	default: return "wasm_ref_defined";
	}
	switch (value.heap) {
	case HeapKind::Func: return "wasm_funcref";
	case HeapKind::Extern: return "wasm_externref";
	case HeapKind::Any: return "wasm_anyref";
	case HeapKind::Eq: return "wasm_eqref";
	case HeapKind::I31: return "wasm_i31ref";
	case HeapKind::Struct: return "wasm_structref";
	case HeapKind::Array: return "wasm_arrayref";
	case HeapKind::Exn: return "wasm_exnref";
	case HeapKind::Cont: return "wasm_contref";
	case HeapKind::NoCont: return "wasm_nocontref";
	case HeapKind::None: return "wasm_nullref";
	case HeapKind::NoFunc: return "wasm_nullfuncref";
	case HeapKind::NoExtern: return "wasm_nullexternref";
	case HeapKind::NoExn: return "wasm_nullexnref";
	case HeapKind::Defined:
		if (value.typeKind == "struct") return "wasm_structref";
		if (value.typeKind == "array") return "wasm_arrayref";
		if (value.typeKind == "func") return "wasm_funcref";
		if (value.typeKind == "cont") return "wasm_contref";
		if (value.typeKind == "import") return "wasm_ref_import";
		return "wasm_ref_defined";
	default: return "wasm_ref_defined";
	}
}

std::string witOpaqueTypeName(std::string_view name)
{
	return "wasmjit_wit_" + sanitize(name);
}

bool witTypeIsAlias(const Json& definition)
{
	return definition.is_object() && definition.contains("kind")
			&& definition.at("kind").is_object()
			&& definition.at("kind").contains("type");
}

void emitWitOpaqueTypes(std::ostringstream& out, const Json& schema, bool cpp)
{
	if (schema.value("frontend", "") != "wit")
		return;
	std::set<std::string> emitted;
	if (!schema.contains("types") || !schema.at("types").is_array())
		return;
	std::size_t index = 0;
	for (const Json& type : schema.at("types")) {
		if (type.is_object() && witTypeIsAlias(type)) {
			++index;
			continue;
		}
		if (!type.is_object() || !type.contains("kind")) {
			++index;
			continue;
		}
		const std::string sourceName = witTypeDisplayName(schema, index);
		++index;
		if (sourceName.empty())
			continue;
		const std::string name = witOpaqueTypeName(sourceName);
		if (!emitted.insert(name).second)
			continue;
		const Json& kind = type.at("kind");
		if (cpp && kind.contains("enum"))
			out << "enum class " << name << " : std::uint32_t;\n";
		else if (cpp && kind.contains("flags"))
			out << "using " << name << " = std::uint32_t;\n";
		else {
			out << (cpp ? "struct " : "typedef struct ") << name;
			if (!cpp)
				out << " " << name;
			out << ";\n";
		}
	}
}

std::string cType(const Json& type)
{
	const ValueType value = ValueType::fromJson(type);
	if (value.packed)
		return value.kind == ValueKind::I8 ? "int8_t" : "int16_t";
	switch (value.kind) {
	case ValueKind::I32:
	case ValueKind::S32: return "int32_t";
	case ValueKind::U32: return "uint32_t";
	case ValueKind::I64:
	case ValueKind::S64: return "int64_t";
	case ValueKind::U64: return "uint64_t";
	case ValueKind::S8:
	case ValueKind::I8: return "int8_t";
	case ValueKind::U8: return "uint8_t";
	case ValueKind::S16:
	case ValueKind::I16: return "int16_t";
	case ValueKind::U16: return "uint16_t";
	case ValueKind::F32: return "float";
	case ValueKind::F64: return "double";
	case ValueKind::Bool: return "int32_t";
	case ValueKind::Char: return "uint32_t";
	case ValueKind::V128: return "wasm_v128";
	case ValueKind::Void: return "void";
	case ValueKind::String:
	case ValueKind::ErrorContext: return "void*";
	case ValueKind::Ref:
		if (value.wit && !value.name.empty())
			return witOpaqueTypeName(value.name) + "*";
		return refTypeName(type) + "*";
	case ValueKind::StringRef:
	case ValueKind::StringViewWtf8:
	case ValueKind::StringViewWtf16:
	case ValueKind::StringViewIter:
		return refTypeName(type) + "*";
	case ValueKind::Unknown: break;
	}
	return "void*";
}

void collectRefs(const Json& node, std::set<std::string>& names)
{
	if (node.is_array()) {
		for (const Json& entry : node)
			collectRefs(entry, names);
		return;
	}
	if (!node.is_object())
		return;
	if (isReference(node)) {
		names.insert(refTypeName(node));
		return;
	}
	for (const char* key : { "params", "results", "element", "fields", "type", "func", "bound" }) {
		if (node.contains(key))
			collectRefs(node.at(key), names);
	}
}

void collectWitTypeNames(const Json& schema, std::set<std::string>& names)
{
	if (schema.value("frontend", "") != "wit"
			|| !schema.contains("types") || !schema.at("types").is_array())
		return;
	std::size_t index = 0;
	for (const Json& definition : schema.at("types")) {
		const std::size_t id = index++;
		if (!definition.is_object() || witTypeIsAlias(definition))
			continue;
		const std::string name = definition.contains("witName")
				? definition.value("witName", std::string())
				: witTypeDisplayName(schema, id);
		if (!name.empty())
			names.insert(witOpaqueTypeName(name));
	}
}

void collectSchemaRefs(const Json& schema, std::set<std::string>& names)
{
	if (schema.contains("types"))
		collectRefs(schema.at("types"), names);
	collectRefs(schema.at("imports"), names);
	collectRefs(schema.at("exports"), names);
	collectWitTypeNames(schema, names);
}

std::string importModule(const Json& entry)
{
	const std::string module = entry.value("module", "");
	return module.empty() ? "component" : module;
}

std::string importName(const Json& entry)
{
	return "wasmjit_" + sanitize(importModule(entry)) + "_"
			+ sanitize(entry.at("name").get<std::string>());
}

std::string commentText(std::string text)
{
	for (char& c : text) {
		if (c == '\n' || c == '\r')
			c = ' ';
	}
	return text;
}

std::string functionDescription(const Json& entry)
{
	return commentText(entry.value("description", ""));
}

std::string importComment(const Json& entry)
{
	std::string text = importModule(entry) + "." + entry.at("name").get<std::string>();
	const std::string description = functionDescription(entry);
	if (!description.empty())
		text += " — " + description;
	return text;
}

std::string exportComment(const Json& entry)
{
	std::string text = entry.at("name").get<std::string>();
	const std::string description = functionDescription(entry);
	if (!description.empty())
		text += " — " + description;
	return text;
}

//! Resolves an optional schema-provided parameter name, falling back to the
//! historical `argN` spelling. The result is safe for an identifier in every
//! target language the schema supports.
std::string argumentName(const Json& entry, std::size_t index)
{
	if (entry.contains("argNames") && entry.at("argNames").is_array()) {
		const Json& names = entry.at("argNames");
		if (index < names.size() && names[index].is_string()) {
			const std::string name = names[index].get<std::string>();
			if (!name.empty())
				return sanitize(name);
		}
	}
	return "arg" + std::to_string(index);
}

const Json& signatureParams(const Json& entry)
{
	if (entry.contains("abi") && entry.at("abi").contains("params"))
		return entry.at("abi").at("params");
	return entry.at("type").at("params");
}

const Json& signatureResults(const Json& entry)
{
	if (entry.contains("abi") && entry.at("abi").contains("results"))
		return entry.at("abi").at("results");
	return entry.at("type").at("results");
}

void emitFunction(std::ostringstream& out, const Json& entry, const std::string& name,
		const char* side, const std::string& attribute)
{
	std::string display = entry.value("module", "");
	if (!display.empty())
		display += ".";
	display += entry.at("name").get<std::string>();

	out << "/* " << side << ": " << display << " */\n";
	if (!functionDescription(entry).empty())
		out << "/* " << functionDescription(entry) << " */\n";
	if (!attribute.empty())
		out << attribute << "\n";
	const Json& results = signatureResults(entry);
	const bool multiValue = results.size() > 1u;
	if (multiValue) {
		out << "/* multi-value result: use a generated result buffer */\n";
		out << "void " << name << "(";
	} else if (results.empty()) {
		out << "void " << name << "(";
	} else {
		out << cType(results[0]) << " " << name << "(";
	}

	const Json& params = signatureParams(entry);
	if (params.empty()) {
		if (!multiValue)
			out << "void";
	} else {
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0)
				out << ", ";
			out << cType(params[i]) << " " << argumentName(entry, i);
			if (isReference(params[i]))
				out << " /* borrowed */";
		}
	}
	if (multiValue) {
		if (!params.empty())
			out << ", ";
		out << "void* /* result buffer */";
	}
	out << ")";
	if (!multiValue && !results.empty() && isReference(results[0]))
		out << " /* ownership: explicit */";
	out << ";\n\n";
}


std::string nonFunctionNotes(const Json& schema);

std::string emitCHeaderImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	const std::string guard =
			"WASMJIT_GENERATED_" + sanitize(schema.value("module", "module")) + "_H";
	std::ostringstream out;
	out << "/* Generated from the " << schema.at("schema").get<std::string>()
		<< " " << schema.at("direction").get<std::string>()
		<< " schema. Do not edit. */\n";
	out << "#ifndef " << guard << "\n";
	out << "#define " << guard << "\n\n";
	out << "#include <stdint.h>\n";
	out << "#include <stddef.h>\n\n";
	out << "#if defined(__wasm__) && (defined(__clang__) || defined(__GNUC__))\n";
	out << "#  define WASMJIT_WASM_IMPORT(module, name) \\\n"
		   "       __attribute__((import_module(module), import_name(name)))\n";
	out << "#else\n";
	out << "#  define WASMJIT_WASM_IMPORT(module, name)\n";
	out << "#endif\n\n";
	out << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
	out << "typedef struct wasm_v128 { uint8_t bytes[16]; } wasm_v128;\n";
	emitWitOpaqueTypes(out, schema, false);
	for (const std::string& ref : refs) {
		if (ref.rfind("wasmjit_wit_", 0) == 0)
			continue;
		out << "typedef struct " << ref << " " << ref << ";\n";
	}
	out << "\n/* Reference types are opaque. Reference parameters are borrowed unless\n"
		   "   the adapter says otherwise; reference results carry an explicit\n"
		   "   ownership note. This header defines no layout, no TypeId and no\n"
		   "   sizeof contract. */\n\n";

	for (const Json& entry : schema.at("exports")) {
		if (entry.value("kind", "") != "function")
			continue;
		emitFunction(out, entry, "wasmjit_" + sanitize(entry.at("name").get<std::string>()),
				"H2G export");
	}
	for (const Json& entry : schema.at("imports")) {
		if (entry.value("kind", "") != "function")
			continue;
		const std::string attribute = "WASMJIT_WASM_IMPORT("
				+ quoteName(importModule(entry), false) + ", "
				+ quoteName(entry.at("name").get<std::string>(), false) + ")";
		emitFunction(out, entry,
				importName(entry),
				"G2H import", attribute);
	}

	out << nonFunctionNotes(schema);
	out << "#ifdef __cplusplus\n}\n#endif\n\n";
	out << "#endif /* " << guard << " */\n";
	return out.str();
}

std::string rustType(const Json& type)
{
	if (type.value("packed", false))
		return type.value("type", "") == "i8" ? "i8" : "i16";
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s32") return "i32";
	if (kind == "u32") return "u32";
	if (kind == "i64" || kind == "s64") return "i64";
	if (kind == "u64") return "u64";
	if (kind == "s8") return "i8";
	if (kind == "u8") return "u8";
	if (kind == "s16") return "i16";
	if (kind == "u16") return "u16";
	if (kind == "f32" || kind == "float32") return "f32";
	if (kind == "f64" || kind == "float64") return "f64";
	if (kind == "bool") return "i32";
	if (kind == "char") return "u32";
	if (kind == "v128") return "wasm_v128";
	if (kind == "void") return "()";
	if (kind == "string" || kind == "error-context")
		return "*mut core::ffi::c_void";
	if (isReference(type))
		return "*mut " + refTypeName(type);
	return "*mut core::ffi::c_void";
}

void emitRustParams(std::ostringstream& out, const Json& entry, bool resultBuffer)
{
	const Json& params = signatureParams(entry);
	bool first = true;
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (!first)
			out << ", ";
		first = false;
		out << argumentName(entry, i) << ": " << rustType(params[i]);
		if (isReference(params[i]))
			out << " /* borrowed */";
	}
	if (resultBuffer) {
		if (!first)
			out << ", ";
		out << "result: *mut core::ffi::c_void";
	}
}

void emitRustReturn(std::ostringstream& out, const Json& results)
{
	if (results.size() != 1u)
		return;
	out << " -> " << rustType(results[0]);
	if (isReference(results[0]))
		out << " /* ownership: explicit */";
}

std::string pascalName(std::string_view name)
{
	std::string out;
	bool upper = true;
	for (const char c : name) {
		if (c == '_' || c == '-') {
			upper = true;
			continue;
		}
		out.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
							: c);
		upper = false;
	}
	if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())))
		out.insert(0, "X");
	return out;
}

std::string refPascal(const Json& type)
{
	return pascalName(refTypeName(type));
}

std::string tsType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "i64" || kind == "f32" || kind == "f64"
			|| kind == "s8" || kind == "u8" || kind == "s16" || kind == "u16"
			|| kind == "s32" || kind == "u32")
		return "number";
	if (kind == "s64" || kind == "u64") return "bigint";
	if (kind == "bool") return "boolean";
	if (kind == "char" || kind == "string") return "string";
	if (kind == "v128") return "Uint8Array";
	if (kind == "void") return "void";
	if (isReference(type)) return refPascal(type);
	return "unknown";
}

std::string asType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s8" || kind == "u8" || kind == "s16"
			|| kind == "u16" || kind == "s32" || kind == "u32")
		return "i32";
	if (kind == "i64" || kind == "s64" || kind == "u64") return "i64";
	if (kind == "f32" || kind == "float32") return "f32";
	if (kind == "f64" || kind == "float64") return "f64";
	if (kind == "bool") return "bool";
	if (kind == "char" || kind == "string") return "string";
	if (kind == "v128") return "v128";
	if (kind == "void") return "void";
	if (kind == "error-context" || isReference(type)) return "externref";
	return "externref";
}

std::string zigType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s32") return "i32";
	if (kind == "u32") return "u32";
	if (kind == "i64" || kind == "s64") return "i64";
	if (kind == "u64") return "u64";
	if (kind == "s8") return "i8";
	if (kind == "u8") return "u8";
	if (kind == "s16") return "i16";
	if (kind == "u16") return "u16";
	if (kind == "f32" || kind == "float32") return "f32";
	if (kind == "f64" || kind == "float64") return "f64";
	if (kind == "bool") return "bool";
	if (kind == "char") return "u32";
	if (kind == "string") return "[]const u8";
	if (kind == "v128") return "wasm_v128";
	if (kind == "void") return "void";
	if (kind == "error-context") return "*anyopaque";
	if (isReference(type)) return "*" + refTypeName(type);
	return "*anyopaque";
}

std::string kotlinType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s8" || kind == "u8" || kind == "s16"
			|| kind == "u16" || kind == "s32" || kind == "u32")
		return "Int";
	if (kind == "i64" || kind == "s64" || kind == "u64") return "Long";
	if (kind == "f32" || kind == "float32") return "Float";
	if (kind == "f64" || kind == "float64") return "Double";
	if (kind == "bool") return "Boolean";
	if (kind == "char") return "Char";
	if (kind == "string") return "String";
	if (kind == "v128") return "WasmV128";
	if (kind == "void") return "Unit";
	if (isReference(type)) return refPascal(type);
	return "Any";
}

std::string moonbitType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s8" || kind == "u8" || kind == "s16"
			|| kind == "u16" || kind == "s32" || kind == "u32")
		return "Int";
	if (kind == "i64" || kind == "s64" || kind == "u64") return "Int64";
	if (kind == "f32" || kind == "float32") return "Float";
	if (kind == "f64" || kind == "float64") return "Double";
	if (kind == "bool") return "Bool";
	if (kind == "char") return "Char";
	if (kind == "string") return "String";
	if (kind == "v128") return "Bytes";
	if (kind == "void") return "Unit";
	if (isReference(type)) return refPascal(type);
	return "Bytes";
}

std::string goType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s32") return "int32";
	if (kind == "u32") return "uint32";
	if (kind == "i64" || kind == "s64") return "int64";
	if (kind == "u64") return "uint64";
	if (kind == "s8") return "int8";
	if (kind == "u8") return "uint8";
	if (kind == "s16") return "int16";
	if (kind == "u16") return "uint16";
	if (kind == "f32" || kind == "float32") return "float32";
	if (kind == "f64" || kind == "float64") return "float64";
	if (kind == "bool") return "bool";
	if (kind == "char") return "rune";
	if (kind == "string") return "string";
	if (kind == "v128") return "[16]byte";
	if (kind == "void") return "";
	// Go's wasmimport/wasmexport accept integer handles, not pointer types
	// (docs/EMBEDDING_SURFACE.md §11.6: prefer a table handle).
	if (kind == "error-context" || isReference(type)) return "uintptr";
	return "uintptr";
}

std::string dartType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "i64" || kind == "s8" || kind == "u8"
			|| kind == "s16" || kind == "u16" || kind == "s32" || kind == "u32"
			|| kind == "s64" || kind == "u64")
		return "int";
	if (kind == "f32" || kind == "f64" || kind == "float32" || kind == "float64")
		return "double";
	if (kind == "bool") return "bool";
	if (kind == "char" || kind == "string") return "String";
	if (kind == "v128") return "Pointer<Uint8>";
	if (kind == "void") return "void";
	if (kind == "error-context" || isReference(type)) return "Pointer<Void>";
	return "Pointer<Void>";
}

// Non-function entries cannot be expressed as a core callable. They are kept
// visible in every generated file as comments so a binding generator does not
// silently drop a value/instance/type/global/memory/table declaration.
std::string nonFunctionNotes(const Json& schema)
{
	std::ostringstream out;
	bool started = false;
	for (const char* section : { "exports", "imports" }) {
		for (const Json& entry : schema.at(section)) {
			const std::string kind = entry.value("kind", "");
			if (kind == "function")
				continue;
			if (!started) {
				out << "\n// Non-function entries (host access is outside the "
					   "generated callable ABI):\n";
				started = true;
			}
			out << "// " << (std::string_view(section) == "exports" ? "export"
					: "import") << ": ";
			if (!kind.empty())
				out << kind << " ";
			out << '"' << entry.value("name", "") << '"';
			const auto type = entry.find("type");
			if (type != entry.end() && type->is_object()) {
				if (type->contains("kind"))
					out << " (" << type->at("kind").get<std::string>() << ")";
				else if (type->contains("type"))
					out << " (" << type->at("type").get<std::string>() << ")";
			} else if (entry.contains("typeIndex")) {
				out << " (type index "
					<< entry.at("typeIndex").get<std::size_t>() << ")";
			}
			out << "\n";
		}
	}
	if (started)
		out << "\n";
	if (schema.value("witAsync", false))
		out << "// WIT async functions are emitted as synchronous callables; no\n"
			   "// async ABI, stream/future protocol or async ownership contract is\n"
			   "// generated yet.\n\n";
	return out.str();
}

std::string emitWitImpl(const Json& schema)
{
	std::ostringstream out;
	const auto witIdent = [](std::string text) {
		std::string normalized;
		for (const char c : text) {
			const char out = c == '_' ? '-' : c;
			if (out == '-' && !normalized.empty() && normalized.back() == '-')
				continue;
			normalized.push_back(out);
		}
		while (!normalized.empty() && normalized.front() == '-')
			normalized.erase(normalized.begin());
		while (!normalized.empty() && normalized.back() == '-')
			normalized.pop_back();
		text = std::move(normalized);
		if (!text.empty() && !std::isalpha(static_cast<unsigned char>(text.front())))
			text.insert(0, "x-");
		static const std::set<std::string> reserved = { "type", "record",
			"variant", "enum", "flags", "resource", "func", "world",
			"interface", "package", "use", "import", "export", "own",
			"borrow", "list", "option", "result", "tuple", "stream",
			"future", "async", "static", "constructor" };
		if (reserved.count(text) != 0)
			text.insert(0, "%");
		return text;
	};
	const auto witName = [&](std::string_view text) {
		return witIdent(sanitize(text));
	};
	std::map<std::size_t, std::string> typeNames;
	std::map<std::string, std::string> refNames;
	std::set<std::string> usedNames;
	if (schema.contains("types") && schema.at("types").is_array()) {
		std::size_t id = 0;
		for (const Json& definition : schema.at("types")) {
			std::string name = definition.contains("witName")
					&& definition.at("witName").is_string()
					? definition.at("witName").get<std::string>() : std::string();
			if (name.empty())
				name = definition.contains("name")
						&& definition.at("name").is_string()
						? definition.at("name").get<std::string>()
						: "type_" + std::to_string(id);
			name = witName(name);
			const bool primitive = name == "bool" || name == "u8" || name == "s8"
					|| name == "u16" || name == "s16" || name == "u32"
					|| name == "s32" || name == "u64" || name == "s64"
					|| name == "f32" || name == "f64" || name == "char"
					|| name == "string" || name == "error-context";
			if (primitive || usedNames.count(name) != 0)
				name = witIdent("wit_" + name + "_" + std::to_string(id));
			usedNames.insert(name);
			typeNames[id] = name;
			const std::string declared = definition.contains("name")
					&& definition.at("name").is_string()
					? definition.at("name").get<std::string>() : std::string();
			if (!declared.empty())
				refNames.emplace(declared, name);
			const std::string display = definition.contains("witName")
					&& definition.at("witName").is_string()
					? definition.at("witName").get<std::string>() : std::string();
			if (!display.empty())
				refNames.emplace(display, name);
			++id;
		}
	}

	auto ref = [&](const Json& type, auto&& self) -> std::string {
		if (type.is_null())
			return "_";
		if (type.is_number_integer()) {
			const auto id = type.get<std::size_t>();
			const auto found = typeNames.find(id);
			return found == typeNames.end() ? "u32" : found->second;
		}
		if (type.is_string()) {
			const std::string name = type.get<std::string>();
			return name == "void" || name == "unit" ? "_" : name;
		}
		if (!type.is_object())
			return "u32";
		if (type.contains("type")) {
			const Json& kind = type.at("type");
			if (kind.is_string() && kind.get<std::string>() == "ref") {
				const std::string name = type.value("name", std::string());
				const auto found = refNames.find(name);
				return found == refNames.end() ? witName(name)
						: found->second;
			}
			return self(kind, self);
		}
		return "u32";
	};

	std::string packageName = schema.value("module", "wasmjit:bindings@0.1.0");
	if (packageName.find(':') == std::string::npos)
		packageName = "wasmjit:bindings@0.1.0";
	const std::string worldName = witName(
			schema.value("world", schema.value("module", "component")));
	out << "package " << packageName << ";\n\n";

	if (schema.contains("types") && schema.at("types").is_array()
			&& !schema.at("types").empty()) {
		out << "interface types {\n";
		std::size_t id = 0;
		for (const Json& definition : schema.at("types")) {
			const std::string name = typeNames[id++];
			if (!definition.contains("kind"))
				continue;
			const Json& kind = definition.at("kind");
			if (kind.is_string()) {
				if (kind.get<std::string>() == "resource")
					out << "  resource " << name << ";\n";
				continue;
			}
			if (!kind.is_object())
				continue;
			if (kind.contains("type")) {
				out << "  type " << name << " = "
					<< ref(kind.at("type"), ref) << ";\n";
			} else if (kind.contains("record")) {
				out << "  record " << name << " {\n";
				const auto fields = kind.at("record").find("fields");
				if (fields != kind.at("record").end() && fields->is_array())
					for (const Json& field : *fields)
						out << "    " << witName(field.value("name", std::string()))
							<< ": " << ref(field.value("type", Json{}), ref) << ",\n";
				out << "  }\n";
			} else if (kind.contains("variant")) {
				out << "  variant " << name << " {\n";
				const auto cases = kind.at("variant").find("cases");
				if (cases != kind.at("variant").end() && cases->is_array())
					for (const Json& alternative : *cases) {
						out << "    " << witName(alternative.value("name", std::string()));
						if (alternative.contains("type")
								&& !alternative.at("type").is_null())
							out << "(" << ref(alternative.at("type"), ref) << ")";
						out << ",\n";
					}
				out << "  }\n";
			} else if (kind.contains("enum")) {
				out << "  enum " << name << " {\n";
				const auto cases = kind.at("enum").find("cases");
				if (cases != kind.at("enum").end() && cases->is_array())
					for (const Json& entry : *cases)
						out << "    " << witName(entry.is_string()
								? entry.get<std::string>()
								: entry.value("name", std::string())) << ",\n";
				out << "  }\n";
			} else if (kind.contains("flags")) {
				out << "  flags " << name << " {\n";
				const auto values = kind.at("flags").find("flags");
				if (values != kind.at("flags").end() && values->is_array())
					for (const Json& entry : *values)
						out << "    " << witName(entry.value("name", std::string()))
							<< ",\n";
				out << "  }\n";
			} else if (kind.contains("tuple")) {
				out << "  type " << name << " = tuple<";
				const auto types = kind.at("tuple").find("types");
				if (types != kind.at("tuple").end() && types->is_array())
					for (std::size_t i = 0; i < types->size(); ++i) {
						if (i != 0) out << ", ";
						out << ref(types->at(i), ref);
					}
				out << ">;\n";
			} else if (kind.contains("list")) {
				out << "  type " << name << " = list<"
					<< ref(kind.at("list"), ref) << ">;\n";
			} else if (kind.contains("option")) {
				out << "  type " << name << " = option<"
					<< ref(kind.at("option"), ref) << ">;\n";
			} else if (kind.contains("result")) {
				const Json& result = kind.at("result");
				const bool hasOk = result.contains("ok") && !result.at("ok").is_null();
				const bool hasErr = result.contains("err") && !result.at("err").is_null();
				out << "  type " << name << " = result";
				if (hasOk || hasErr) {
					out << "<"
						<< (hasOk ? ref(result.at("ok"), ref) : std::string("_"))
						<< ", "
						<< (hasErr ? ref(result.at("err"), ref) : std::string("_"))
						<< ">";
				}
				out << ";\n";
			} else if (kind.contains("handle")) {
				const Json& handle = kind.at("handle");
				out << "  type " << name << " = "
					<< (handle.contains("own") ? "own<" : "borrow<")
					<< ref(handle.contains("own") ? handle.at("own")
							: handle.at("borrow"), ref) << ">;\n";
			} else if (kind.contains("stream")) {
				out << "  type " << name << " = stream<"
					<< ref(kind.at("stream"), ref) << ">;\n";
			} else if (kind.contains("future")) {
				out << "  type " << name << " = future<"
					<< ref(kind.at("future"), ref) << ">;\n";
			}
		}
		out << "}\n\n";
	}

	const auto emitFunction = [&](const Json& entry) -> std::string {
		std::ostringstream fn;
		fn << witName(entry.value("name", std::string())) << ": func(";
		const Json& logical = entry.contains("type") ? entry.at("type")
				: Json::object();
		const Json& params = logical.contains("params") ? logical.at("params")
				: Json::array();
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0)
				fn << ", ";
			std::string name = "arg" + std::to_string(i);
			if (entry.contains("argNames") && entry.at("argNames").is_array()
					&& i < entry.at("argNames").size()
					&& entry.at("argNames")[i].is_string())
				name = entry.at("argNames")[i].get<std::string>();
			fn << witName(name) << ": " << ref(params[i], ref);
		}
		fn << ")";
		if (logical.contains("results") && logical.at("results").is_array()
				&& logical.at("results").size() == 1) {
			const Json& result = logical.at("results")[0];
			const bool isVoid = result.is_string()
					? result.get<std::string>() == "void"
					: result.is_object() && result.value("type", "") == "void";
			if (!isVoid)
				fn << " -> " << ref(result, ref);
		}
		fn << ";\n";
		return fn.str();
	};

	std::map<std::string, std::vector<const Json*>> interfaceImports;
	std::map<std::string, std::vector<const Json*>> interfaceExports;
	std::vector<const Json*> worldImports;
	std::vector<const Json*> worldExports;
	if (schema.contains("imports") && schema.at("imports").is_array())
		for (const Json& entry : schema.at("imports")) {
			if (entry.value("kind", "") != "function")
				continue;
			const std::string module = entry.value("module", std::string());
			if (module.empty())
				worldImports.push_back(&entry);
			else
				interfaceImports[witName(module)].push_back(&entry);
		}
	if (schema.contains("exports") && schema.at("exports").is_array())
		for (const Json& entry : schema.at("exports")) {
			if (entry.value("kind", "") != "function")
				continue;
			const std::string module = entry.value("module", std::string());
			if (module.empty())
				worldExports.push_back(&entry);
			else
				interfaceExports[witName(module)].push_back(&entry);
		}

	const auto emitUseTypes = [&](std::ostringstream& target,
			std::string_view indent) {
		if (typeNames.empty())
			return;
		target << indent << "use types.{";
		bool first = true;
		for (const auto& [ignored, type] : typeNames) {
			(void)ignored;
			if (!first)
				target << ", ";
			target << type;
			first = false;
		}
		target << "};\n";
	};

	for (const auto& [name, entries] : interfaceImports) {
		out << "interface " << name << " {\n";
		emitUseTypes(out, "  ");
		for (const Json* entry : entries)
			out << "  " << emitFunction(*entry);
		out << "}\n\n";
	}
	for (const auto& [name, entries] : interfaceExports) {
		out << "interface " << name << " {\n";
		emitUseTypes(out, "  ");
		for (const Json* entry : entries)
			out << "  " << emitFunction(*entry);
		out << "}\n\n";
	}

	out << "world " << worldName << " {\n";
	emitUseTypes(out, "  ");
	for (const Json* entry : worldImports)
		out << "  import " << emitFunction(*entry);
	for (const Json* entry : worldExports)
		out << "  export " << emitFunction(*entry);
	for (const auto& [name, ignored] : interfaceImports) {
		(void)ignored;
		out << "  import " << name << ";\n";
	}
	for (const auto& [name, ignored] : interfaceExports) {
		(void)ignored;
		out << "  export " << name << ";\n";
	}
	out << "}\n";
	return out.str();
}

std::string emitWitxImpl(const Json& schema)
{
	const auto witxType = [](const Json& type) -> std::string {
		if (type.is_string())
			return type.get<std::string>();
		if (!type.is_object())
			return "i32";
		const std::string kind = type.value("type", "");
		if (kind == "ref")
			return "i32";
		if (kind == "void")
			return "i32";
		return kind.empty() ? "i32" : kind;
	};
	std::ostringstream out;
	out << "(module $" << sanitize(schema.value("module", "module")) << "\n";
	if (schema.contains("witxTypes") && schema.at("witxTypes").is_array())
		for (const Json& definition : schema.at("witxTypes"))
			if (definition.is_string())
				out << "  " << definition.get<std::string>() << "\n";
	const auto emitFunction = [&](const Json& entry) {
		if (entry.contains("witxSource") && entry.at("witxSource").is_string()) {
			out << "  " << entry.at("witxSource").get<std::string>() << "\n";
			return;
		}
		out << "  (@interface func (export \""
			<< entry.value("name", std::string()) << "\")\n";
		const Json& logical = entry.contains("type") ? entry.at("type")
				: Json::object();
		const Json& params = logical.contains("params") ? logical.at("params")
				: Json::array();
		for (std::size_t i = 0; i < params.size(); ++i) {
			std::string name = "arg" + std::to_string(i);
			if (entry.contains("argNames") && entry.at("argNames").is_array()
					&& i < entry.at("argNames").size()
					&& entry.at("argNames")[i].is_string())
				name = entry.at("argNames")[i].get<std::string>();
			out << "    (param $" << sanitize(name) << " "
				<< witxType(params[i]) << ")\n";
		}
		const Json& results = logical.contains("results") ? logical.at("results")
				: Json::array();
		for (std::size_t i = 0; i < results.size(); ++i) {
			std::string name = "result" + std::to_string(i);
			if (entry.contains("resultNames")
					&& entry.at("resultNames").is_array()
					&& i < entry.at("resultNames").size()
					&& entry.at("resultNames")[i].is_string())
				name = entry.at("resultNames")[i].get<std::string>();
			out << "    (result $" << sanitize(name) << " "
				<< witxType(results[i]) << ")\n";
		}
		out << "  )\n";
	};
	if (schema.contains("imports") && schema.at("imports").is_array())
		for (const Json& entry : schema.at("imports"))
			if (entry.value("kind", "") == "function")
				emitFunction(entry);
	if (schema.contains("exports") && schema.at("exports").is_array())
		for (const Json& entry : schema.at("exports"))
			if (entry.value("kind", "") == "function")
				emitFunction(entry);
	out << ")\n";
	return out.str();
}

} // namespace WASM::Bindings::detail
