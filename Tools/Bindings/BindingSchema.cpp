#include "BindingSchema.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// docs/EMBEDDING_SURFACE.md §12.8: the neutral binding schema and the first
// C/C++ and Rust emitters. This is tooling, not an engine header.
namespace WASM::Bindings {

namespace {

using Json = nlohmann::ordered_json;


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
	case WASM::ValueTypeCode::StringRef: return "stringref";
	case WASM::ValueTypeCode::StringViewWtf8: return "stringview_wtf8";
	case WASM::ValueTypeCode::StringViewWtf16: return "stringview_wtf16";
	case WASM::ValueTypeCode::StringViewIter: return "stringview_iter";
	case WASM::ValueTypeCode::Void: return "void";
	default: return "unknown";
	}
}

const char* abstractHeapName(WASM::AbstractHeapType heap)
{
	switch (heap) {
	case WASM::AbstractHeapType::Func: return "func";
	case WASM::AbstractHeapType::Extern: return "extern";
	case WASM::AbstractHeapType::Any: return "any";
	case WASM::AbstractHeapType::Eq: return "eq";
	case WASM::AbstractHeapType::I31: return "i31";
	case WASM::AbstractHeapType::Struct: return "struct";
	case WASM::AbstractHeapType::Array: return "array";
	case WASM::AbstractHeapType::Exn: return "exn";
	case WASM::AbstractHeapType::Cont: return "cont";
	case WASM::AbstractHeapType::NoCont: return "nocont";
	case WASM::AbstractHeapType::None: return "none";
	case WASM::AbstractHeapType::NoFunc: return "nofunc";
	case WASM::AbstractHeapType::NoExtern: return "noextern";
	case WASM::AbstractHeapType::NoExn: return "noexn";
	}
	return "unknown";
}

Json heapJson(const WASM::CanonHeapType& heap, const WASM::TypeRegistry& types)
{
	Json out;
	switch (heap.kind) {
	case WASM::CanonHeapType::Kind::Abstract:
		out["heap"] = abstractHeapName(heap.abstract);
		break;
	case WASM::CanonHeapType::Kind::Global:
		out["heap"] = "defined";
		out["typeId"] = heap.id.value;
		switch (types.canonicalType(heap.id).kind()) {
		case WASM::CanonTypeKind::Func: out["typeKind"] = "func"; break;
		case WASM::CanonTypeKind::Struct: out["typeKind"] = "struct"; break;
		case WASM::CanonTypeKind::Array: out["typeKind"] = "array"; break;
		case WASM::CanonTypeKind::Cont: out["typeKind"] = "cont"; break;
		case WASM::CanonTypeKind::Import: out["typeKind"] = "import"; break;
		}
		break;
	case WASM::CanonHeapType::Kind::Rec:
		out["heap"] = "rec";
		out["memberIndex"] = heap.memberIndex;
		break;
	}
	if (heap.isExact)
		out["exact"] = true;
	return out;
}

Json valueTypeJson(const WASM::CanonValueType& type, const WASM::TypeRegistry& types)
{
	if (type.opcode == WASM::ValueTypeCode::Ref || type.opcode == WASM::ValueTypeCode::RefNull) {
		Json out = heapJson(type.heap, types);
		out["type"] = "ref";
		out["nullable"] = type.nullable;
		if (type.isShared)
			out["shared"] = true;
		return out;
	}
	WASM::AbstractHeapType shorthand;
	if (WASM::shorthandRefHeapType(type.opcode, shorthand)) {
		Json out;
		out["type"] = "ref";
		out["heap"] = abstractHeapName(shorthand);
		out["nullable"] = true;
		return out;
	}
	Json out;
	out["type"] = valueTypeName(type.opcode);
	if (type.isShared)
		out["shared"] = true;
	return out;
}
Json valueTypeJson(const WASM::ValueType& type)
{
	WASM::AbstractHeapType shorthand;
	if (WASM::shorthandRefHeapType(type.opcode, shorthand)) {
		Json out;
		out["type"] = "ref";
		out["heap"] = abstractHeapName(shorthand);
		out["nullable"] = true;
		return out;
	}
	if (type.opcode == WASM::ValueTypeCode::Ref || type.opcode == WASM::ValueTypeCode::RefNull) {
		Json out;
		out["type"] = "ref";
		out["nullable"] = type.opcode == WASM::ValueTypeCode::RefNull;
		if (type.heapType < 0) {
			out["heap"] = abstractHeapName(static_cast<WASM::AbstractHeapType>(type.heapType));
		} else {
			out["heap"] = "local";
			out["typeIndex"] = static_cast<uint32_t>(type.heapType);
		}
		if (type.isExact)
			out["exact"] = true;
		if (type.isShared)
			out["shared"] = true;
		return out;
	}
	Json out;
	out["type"] = valueTypeName(type.opcode);
	return out;
}

Json storageTypeJson(const WASM::CanonStorageType& storage, const WASM::TypeRegistry& types)
{
	if (storage.isPacked) {
		Json out;
		out["type"] = valueTypeName(storage.packed);
		out["packed"] = true;
		return out;
	}
	return valueTypeJson(storage.value, types);
}

Json fieldTypeJson(const WASM::CanonFieldType& field, const WASM::TypeRegistry& types)
{
	Json out;
	out["type"] = storageTypeJson(field.storage, types);
	out["mutable"] = field.isMutable;
	return out;
}

Json typeJson(const WASM::CoreTypeView& view, const WASM::TypeRegistry& types)
{
	Json out;
	switch (view.kind()) {
	case WASM::CanonTypeKind::Func:
		out["kind"] = "func";
		out["params"] = Json::array();
		for (const WASM::CanonStorageType& param : view.funcParams())
			out["params"].push_back(storageTypeJson(param, types));
		out["results"] = Json::array();
		for (const WASM::CanonStorageType& result : view.funcResults())
			out["results"].push_back(storageTypeJson(result, types));
		break;
	case WASM::CanonTypeKind::Struct:
		out["kind"] = "struct";
		out["fields"] = Json::array();
		for (const WASM::CanonFieldType& field : view.structFields())
			out["fields"].push_back(fieldTypeJson(field, types));
		break;
	case WASM::CanonTypeKind::Array:
		out["kind"] = "array";
		out["element"] = fieldTypeJson(view.arrayElement(), types);
		break;
	case WASM::CanonTypeKind::Cont:
		out["kind"] = "cont";
		out["func"] = heapJson(view.contFunc(), types);
		break;
	case WASM::CanonTypeKind::Import:
		out["kind"] = "import";
		out["bound"] = heapJson(view.importBound(), types);
		break;
	}
	return out;
}


Json limitsJson(const WASM::Limits& limits, const char* unit)
{
	Json out;
	out["unit"] = unit;
	out["initial"] = limits.initial;
	if (limits.maximum)
		out["maximum"] = *limits.maximum;
	return out;
}

//! Adds the optional editorial fields an AI or human may fill after exporting
//! a schema. They are intentionally empty by default.
void addFunctionEditorialFields(Json& out)
{
	out["description"] = "";
	out["argNames"] = Json::array();
}

void ensureFunctionEditorialFields(Json& out)
{
	if (out.value("kind", "") == "function" && !out.contains("description"))
		addFunctionEditorialFields(out);
}

Json descriptorJson(const WASM::ExternTypeDescriptor& type, const WASM::TypeRegistry& types)
{
	Json out;
	switch (type.kind) {
	case WASM::ExternalKind::Function:
		out["kind"] = "function";
		out["type"] = typeJson(types.canonicalType(type.typeId), types);
		addFunctionEditorialFields(out);
		break;
	case WASM::ExternalKind::Table:
		out["kind"] = "table";
		out["element"] = valueTypeJson(type.tableType.elementType);
		out["limits"] = limitsJson(type.tableType.limits, "entries");
		out["shared"] = type.tableType.isShared;
		break;
	case WASM::ExternalKind::Memory:
		out["kind"] = "memory";
		out["index"] = type.memoryType.indexType == WASM::MemoryIndexType::I64 ? "i64" : "i32";
		out["limits"] = limitsJson(type.memoryType.limits, "pages");
		out["pageSize"] = type.memoryType.pageSize;
		break;
	case WASM::ExternalKind::Global:
		out["kind"] = "global";
		out["type"] = valueTypeJson(type.globalType.contentType);
		out["mutable"] = type.globalType.isMutable;
		out["shared"] = type.globalType.isShared;
		if (!type.globalType.contentId.isNone())
			out["contentTypeId"] = type.globalType.contentId.value;
		break;
	case WASM::ExternalKind::Tag:
		out["kind"] = "tag";
		out["attribute"] = type.tagAttribute;
		if (!type.typeId.isNone())
			out["type"] = typeJson(types.canonicalType(type.typeId), types);
		break;
	case WASM::ExternalKind::Type:
		out["kind"] = "type";
		if (!type.typeId.isNone())
			out["type"] = typeJson(types.canonicalType(type.typeId), types);
		break;
	}
	if (type.isExact)
		out["exact"] = true;
	return out;
}

Json moduleSchemaImpl(const WASM::Module& module, const WASM::TypeRegistry& types,
		Direction direction, std::string_view moduleName)
{
	Json schema;
	schema["schema"] = "wasmjit.binding/1";
	schema["direction"] = directionName(direction);
	schema["module"] = moduleName;
	schema["types"] = Json::array();
	for (uint32_t i = 0; i < module.typeCount(); ++i) {
		Json type = typeJson(module.typeView(i), types);
		type["index"] = i;
		schema["types"].push_back(type);
	}
	schema["imports"] = Json::array();
	if (direction == Direction::GuestToHost || direction == Direction::Complete) {
		for (const WASM::ImportDescriptor& descriptor : module.importDescriptors()) {
			Json import = descriptorJson(descriptor.type, types);
			import["module"] = descriptor.moduleName;
			import["name"] = descriptor.fieldName;
			schema["imports"].push_back(import);
		}
	} else if (direction == Direction::GuestToGuest) {
		// A guest-to-guest consumer imports the *producer's exports*. The
		// producer's own imports are irrelevant to that consumer.
		for (const WASM::ExportDescriptor& descriptor : module.exportDescriptors()) {
			Json import = descriptorJson(descriptor.type, types);
			import["module"] = moduleName;
			import["name"] = descriptor.name;
			schema["imports"].push_back(import);
		}
	}
	schema["exports"] = Json::array();
	if (direction == Direction::HostToGuest || direction == Direction::Complete) {
		for (const WASM::ExportDescriptor& descriptor : module.exportDescriptors()) {
			Json exportEntry = descriptorJson(descriptor.type, types);
			exportEntry["name"] = descriptor.name;
			schema["exports"].push_back(exportEntry);
		}
	}
	return schema;
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
	if (!type.is_object() || !type.contains("type") || !type.at("type").is_string())
		return false;
	const std::string kind = type.at("type").get<std::string>();
	return kind == "ref" || kind == "stringref" || kind.rfind("stringview_", 0) == 0;
}

std::string refTypeName(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "stringref")
		return "wasm_stringref";
	if (kind == "stringview_wtf8")
		return "wasm_stringview_wtf8";
	if (kind == "stringview_wtf16")
		return "wasm_stringview_wtf16";
	if (kind == "stringview_iter")
		return "wasm_stringview_iter";
	const std::string heap = type.at("heap").get<std::string>();
	if (heap == "func") return "wasm_funcref";
	if (heap == "extern") return "wasm_externref";
	if (heap == "any") return "wasm_anyref";
	if (heap == "eq") return "wasm_eqref";
	if (heap == "i31") return "wasm_i31ref";
	if (heap == "struct") return "wasm_structref";
	if (heap == "array") return "wasm_arrayref";
	if (heap == "exn") return "wasm_exnref";
	if (heap == "cont") return "wasm_contref";
	if (heap == "nocont") return "wasm_nocontref";
	if (heap == "none") return "wasm_nullref";
	if (heap == "nofunc") return "wasm_nullfuncref";
	if (heap == "noextern") return "wasm_nullexternref";
	if (heap == "noexn") return "wasm_nullexnref";
	if (heap == "defined") {
		const std::string kind = type.value("typeKind", "");
		if (kind == "struct") return "wasm_structref";
		if (kind == "array") return "wasm_arrayref";
		if (kind == "func") return "wasm_funcref";
		if (kind == "cont") return "wasm_contref";
		if (kind == "import") return "wasm_ref_import";
		return "wasm_ref_defined";
	}
	return "wasm_ref_defined";
}

std::string cType(const Json& type)
{
	if (type.value("packed", false))
		return type.value("type", "") == "i8" ? "int8_t" : "int16_t";
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "s32") return "int32_t";
	if (kind == "u32") return "uint32_t";
	if (kind == "i64" || kind == "s64") return "int64_t";
	if (kind == "u64") return "uint64_t";
	if (kind == "s8") return "int8_t";
	if (kind == "u8") return "uint8_t";
	if (kind == "s16") return "int16_t";
	if (kind == "u16") return "uint16_t";
	if (kind == "f32" || kind == "float32") return "float";
	if (kind == "f64" || kind == "float64") return "double";
	if (kind == "bool") return "int32_t";
	if (kind == "char") return "uint32_t";
	if (kind == "v128") return "wasm_v128";
	if (kind == "void") return "void";
	if (kind == "string" || kind == "error-context") return "void*";
	if (isReference(type))
		return refTypeName(type) + "*";
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

void collectSchemaRefs(const Json& schema, std::set<std::string>& names)
{
	if (schema.contains("types"))
		collectRefs(schema.at("types"), names);
	collectRefs(schema.at("imports"), names);
	collectRefs(schema.at("exports"), names);
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
		const char* side, const std::string& attribute = {})
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
	for (const std::string& ref : refs)
		out << "typedef struct " << ref << " " << ref << ";\n";
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
	// (docs/EMBEDDING_SURFACE.md §12.6: prefer a table handle).
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

template <typename Fn>
void forEachFunction(const Json& schema, const char* key, Fn fn)
{
	for (const Json& entry : schema.at(key)) {
		if (entry.value("kind", "") != "function")
			continue;
		fn(entry);
	}
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
	return out.str();
}

} // namespace

const char* directionName(Direction direction) noexcept
{
	switch (direction) {
	case Direction::GuestToHost: return "g2h";
	case Direction::HostToGuest: return "h2g";
	case Direction::GuestToGuest: return "g2g";
	case Direction::Complete: return "all";
	}
	return "g2h";
}

Json moduleSchema(const WASM::Module& module, const WASM::TypeRegistry& types,
		Direction direction, std::string_view moduleName)
{
	return moduleSchemaImpl(module, types, direction, moduleName);
}

namespace {

using Json = nlohmann::ordered_json;

const char* primitiveName(WASM::PrimitiveValueType primitive)
{
	switch (primitive) {
	case WASM::PrimitiveValueType::Bool: return "bool";
	case WASM::PrimitiveValueType::S8: return "s8";
	case WASM::PrimitiveValueType::U8: return "u8";
	case WASM::PrimitiveValueType::S16: return "s16";
	case WASM::PrimitiveValueType::U16: return "u16";
	case WASM::PrimitiveValueType::S32: return "s32";
	case WASM::PrimitiveValueType::U32: return "u32";
	case WASM::PrimitiveValueType::S64: return "s64";
	case WASM::PrimitiveValueType::U64: return "u64";
	case WASM::PrimitiveValueType::F32: return "float32";
	case WASM::PrimitiveValueType::F64: return "float64";
	case WASM::PrimitiveValueType::Char: return "char";
	case WASM::PrimitiveValueType::String: return "string";
	case WASM::PrimitiveValueType::ErrorContext: return "error-context";
	case WASM::PrimitiveValueType::Invalid: return "invalid";
	}
	return "invalid";
}

Json abiCoreTypeJson(WASM::AbiCoreType type)
{
	switch (type) {
	case WASM::AbiCoreType::I32: return Json{{"type", "i32"}};
	case WASM::AbiCoreType::I64: return Json{{"type", "i64"}};
	case WASM::AbiCoreType::F32: return Json{{"type", "f32"}};
	case WASM::AbiCoreType::F64: return Json{{"type", "f64"}};
	}
	return Json{{"type", "i32"}};
}

Json canonicalAbiJson(const WASM::TypeStore& store, WASM::TypeId id)
{
	const WASM::CanonicalAbiSignature* abi = store.canonicalAbi(id, {});
	if (abi == nullptr)
		return Json();
	Json out;
	out["params"] = Json::array();
	for (const WASM::AbiCoreType type : abi->coreParams)
		out["params"].push_back(abiCoreTypeJson(type));
	out["results"] = Json::array();
	for (const WASM::AbiCoreType type : abi->coreResults)
		out["results"].push_back(abiCoreTypeJson(type));
	out["indirectParams"] = abi->indirectParams;
	out["indirectResults"] = abi->indirectResults;
	return out;
}

Json componentValueJson(const WASM::ComponentValueType& type,
		const WASM::Component& component, const WASM::TypeStore& store, unsigned depth);

Json componentTypeJson(const WASM::ComponentTypeView& view,
		const WASM::Component& component, const WASM::TypeStore& store, unsigned depth)
{
	if (depth > 32)
		return Json{{"kind", "recursive"}};

	Json out;
	switch (view.kind()) {
	case WASM::TypeKind::ComponentPrimitive:
		out["type"] = primitiveName(view.primitive());
		break;
	case WASM::TypeKind::ComponentRecord:
		out["kind"] = "record";
		out["fields"] = Json::array();
		for (const WASM::ComponentField& field : view.fields()) {
			Json entry;
			entry["name"] = std::string(store.string(field.name));
			entry["type"] = componentValueJson(field.type, component, store, depth + 1);
			out["fields"].push_back(entry);
		}
		break;
	case WASM::TypeKind::ComponentVariant:
		out["kind"] = "variant";
		out["cases"] = Json::array();
		for (const WASM::ComponentCase& item : view.cases()) {
			Json entry;
			entry["label"] = std::string(store.string(item.label));
			if (item.hasType)
				entry["type"] = componentValueJson(item.type, component, store, depth + 1);
			out["cases"].push_back(entry);
		}
		break;
	case WASM::TypeKind::ComponentList:
		out["kind"] = "list";
		out["element"] = componentValueJson(view.element(), component, store, depth + 1);
		break;
	case WASM::TypeKind::ComponentFixedList:
		out["kind"] = "list";
		out["length"] = view.fixedLength();
		out["element"] = componentValueJson(view.element(), component, store, depth + 1);
		break;
	case WASM::TypeKind::ComponentTuple:
		out["kind"] = "tuple";
		out["elements"] = Json::array();
		for (const WASM::ComponentValueType& item : view.tuple())
			out["elements"].push_back(componentValueJson(item, component, store, depth + 1));
		break;
	case WASM::TypeKind::ComponentFlags:
	case WASM::TypeKind::ComponentEnum:
		out["kind"] = view.kind() == WASM::TypeKind::ComponentFlags ? "flags" : "enum";
		out["labels"] = Json::array();
		for (const WASM::StringId label : view.labels())
			out["labels"].push_back(std::string(store.string(label)));
		break;
	case WASM::TypeKind::ComponentOption:
		out["kind"] = "option";
		out["element"] = componentValueJson(view.element(), component, store, depth + 1);
		break;
	case WASM::TypeKind::ComponentResult:
		out["kind"] = "result";
		if (view.hasResultOk())
			out["ok"] = componentValueJson(view.resultOk(), component, store, depth + 1);
		if (view.hasResultError())
			out["error"] = componentValueJson(view.resultError(), component, store, depth + 1);
		break;
	case WASM::TypeKind::ComponentOwn:
	case WASM::TypeKind::ComponentBorrow:
		out["kind"] = view.kind() == WASM::TypeKind::ComponentOwn ? "own" : "borrow";
		out["typeRef"] = view.typeRef();
		break;
	case WASM::TypeKind::ComponentStream:
	case WASM::TypeKind::ComponentFuture:
		out["kind"] = view.kind() == WASM::TypeKind::ComponentStream ? "stream" : "future";
		if (view.hasStreamElement())
			out["element"] = componentValueJson(view.streamElement(), component, store, depth + 1);
		break;
	case WASM::TypeKind::ComponentMap:
		out["kind"] = "map";
		out["key"] = componentValueJson(view.mapKey(), component, store, depth + 1);
		out["value"] = componentValueJson(view.mapValue(), component, store, depth + 1);
		break;
	case WASM::TypeKind::ComponentFunc:
		out["kind"] = "func";
		out["params"] = Json::array();
		for (const WASM::ComponentFuncParam& param : view.params()) {
			Json entry = componentValueJson(param.type, component, store, depth + 1);
			if (entry.is_object())
				entry["name"] = std::string(store.string(param.name));
			out["params"].push_back(entry);
		}
		out["results"] = Json::array();
		for (const WASM::ComponentValueType& result : view.results())
			out["results"].push_back(componentValueJson(result, component, store, depth + 1));
		break;
	case WASM::TypeKind::ComponentInstance:
	case WASM::TypeKind::ComponentComponent:
		out["kind"] =
				view.kind() == WASM::TypeKind::ComponentInstance ? "instance" : "component";
		out["declarations"] = Json::array();
		for (const WASM::ComponentDeclRecord& decl : view.declarations()) {
			Json entry;
			entry["name"] = std::string(store.string(decl.name));
			entry["sort"] = decl.sort;
			if (!decl.type.isNone())
				entry["typeId"] = decl.type.value;
			out["declarations"].push_back(entry);
		}
		break;
	case WASM::TypeKind::ComponentResource:
		out["kind"] = "resource";
		break;
	default:
		out["kind"] = "core";
		break;
	}
	return out;
}

Json componentValueJson(const WASM::ComponentValueType& type,
		const WASM::Component& component, const WASM::TypeStore& store, unsigned depth)
{
	if (depth > 32)
		return Json{{"kind", "recursive"}};
	switch (type.kind) {
	case WASM::ComponentValueType::Kind::Primitive:
		return Json{{"type", primitiveName(type.primitive)}};
	case WASM::ComponentValueType::Kind::TypeId:
		return componentTypeJson(
				store.componentType(WASM::TypeId{type.typeIndex}), component, store, depth + 1);
	case WASM::ComponentValueType::Kind::TypeIndex:
		if (type.typeIndex < component.types.size()) {
			const WASM::ComponentTypeDef& def = component.types[type.typeIndex];
			if (!def.id.isNone())
				return componentTypeJson(
						store.componentType(def.id), component, store, depth + 1);
		}
		return Json{{"kind", "type-index"}, {"index", type.typeIndex}};
	}
	return Json{{"kind", "unknown"}};
}

void fillComponentExtern(Json& out, const WASM::ExternType& type,
		const WASM::Component& component, const WASM::TypeStore& store, unsigned depth)
{
	switch (type.kind) {
	case WASM::ExternType::Kind::Func:
		out["kind"] = "function";
		addFunctionEditorialFields(out);
		if (type.typeIndex < component.types.size()
				&& !component.types[type.typeIndex].id.isNone()) {
			const WASM::TypeId id = component.types[type.typeIndex].id;
			out["type"] = componentTypeJson(
					store.componentType(id), component, store, depth + 1);
			Json abi = canonicalAbiJson(store, id);
			if (!abi.is_null())
				out["abi"] = abi;
		} else {
			out["typeIndex"] = type.typeIndex;
		}
		break;
	case WASM::ExternType::Kind::Value:
		out["kind"] = "value";
		if (type.value.kind == WASM::ValueBound::Kind::Type)
			out["type"] = componentValueJson(type.value.type, component, store, depth + 1);
		else
			out["valueIndex"] = type.value.valueIndex;
		break;
	case WASM::ExternType::Kind::Instance:
		out["kind"] = "instance";
		out["typeIndex"] = type.typeIndex;
		break;
	case WASM::ExternType::Kind::Component:
		out["kind"] = "component";
		out["typeIndex"] = type.typeIndex;
		break;
	case WASM::ExternType::Kind::CoreModule:
		out["kind"] = "core-module";
		out["typeIndex"] = type.typeIndex;
		break;
	case WASM::ExternType::Kind::Type:
		out["kind"] = "type";
		out["typeIndex"] = type.typeBound.typeIndex;
		break;
	}
}

Json componentExportJson(const WASM::ComponentExport& exportDecl,
		const WASM::Component& component, const WASM::ComponentIndexSpaces& spaces,
		const WASM::TypeStore& store)
{
	Json out;
	out["name"] = exportDecl.name.name;
	out["sort"] = static_cast<unsigned>(exportDecl.sortIndex.sort);
	if (exportDecl.sortIndex.core)
		out["coreSort"] = static_cast<unsigned>(exportDecl.sortIndex.core->sort);

	if (exportDecl.type) {
		fillComponentExtern(out, *exportDecl.type, component, store, 0);
		return out;
	}

	const std::optional<WASM::ComponentDefinitionRef> ref =
			spaces.resolve(exportDecl.sortIndex);
	if (ref) {
		const WASM::Component& owner = *ref->owner;
		if (ref->kind == WASM::ComponentDefinitionKind::Type
				&& ref->index < owner.types.size()) {
			const WASM::ComponentTypeDef& def = owner.types[ref->index];
			if (!def.id.isNone()) {
				out["kind"] = def.kind == WASM::ComponentTypeDef::Kind::Func
						? "function"
						: "type";
				out["type"] = componentTypeJson(
						store.componentType(def.id), owner, store, 0);
				if (def.kind == WASM::ComponentTypeDef::Kind::Func) {
					Json abi = canonicalAbiJson(store, def.id);
					if (!abi.is_null())
						out["abi"] = abi;
				}
			}
		} else if (ref->kind == WASM::ComponentDefinitionKind::Canon
				&& ref->index < owner.canons.size()) {
			const WASM::Canon& canon = owner.canons[ref->index];
			if (canon.typeIndex < owner.types.size()
					&& !owner.types[canon.typeIndex].id.isNone()) {
				const WASM::TypeId id = owner.types[canon.typeIndex].id;
				out["kind"] = "function";
				out["type"] = componentTypeJson(
						store.componentType(id), owner, store, 0);
				Json abi = canonicalAbiJson(store, id);
				if (!abi.is_null())
					out["abi"] = abi;
			}
		} else if (ref->kind == WASM::ComponentDefinitionKind::Import
				&& ref->index < owner.imports.size()) {
			fillComponentExtern(out, owner.imports[ref->index].type, owner, store, 0);
		}
	}
	ensureFunctionEditorialFields(out);
	return out;
}

Json componentImportJson(const WASM::ImportDecl& importDecl,
		const WASM::Component& component, const WASM::TypeStore& store)
{
	Json out;
	out["name"] = importDecl.name.name;
	fillComponentExtern(out, importDecl.type, component, store, 0);
	ensureFunctionEditorialFields(out);
	return out;
}

} // namespace

Json componentSchema(const WASM::Component& component, const WASM::TypeRegistry& types,
		Direction direction, std::string_view componentName)
{
	const WASM::TypeStore& store = types.store();
	const WASM::ComponentIndexSpaces spaces = component.buildIndexSpaces();

	Json schema;
	schema["schema"] = "wasmjit.binding/1";
	schema["kind"] = "component";
	schema["direction"] = directionName(direction);
	schema["module"] = componentName;
	schema["componentName"] = component.componentName();
	schema["imports"] = Json::array();
	if (direction == Direction::GuestToHost || direction == Direction::Complete) {
		for (const WASM::ImportDecl& importDecl : component.imports)
			schema["imports"].push_back(
					componentImportJson(importDecl, component, store));
	} else if (direction == Direction::GuestToGuest) {
		// The consumer imports the producer component's exports. Do not project
		// the producer's own imports into a G2G interface.
		for (const WASM::ComponentExport& exportDecl : component.exports) {
			Json import = componentExportJson(exportDecl, component, spaces, store);
			import["module"] = componentName;
			schema["imports"].push_back(import);
		}
	}
	schema["exports"] = Json::array();
	if (direction == Direction::HostToGuest || direction == Direction::Complete) {
		for (const WASM::ComponentExport& exportDecl : component.exports)
			schema["exports"].push_back(
					componentExportJson(exportDecl, component, spaces, store));
	}
	return schema;
}

Json projectSchema(const Json& input, Direction direction)
{
	if (!input.is_object())
		throw std::runtime_error("schema import: expected a JSON object");
	if (input.value("schema", "") != "wasmjit.binding/1")
		throw std::runtime_error("schema import: unsupported schema id");
	const std::string source = input.value("direction", "");
	const std::string target = directionName(direction);
	if (source.empty())
		throw std::runtime_error("schema import: missing direction");
	if (source == target)
		return input;
	if (source != "all")
		throw std::runtime_error("schema import: direction '" + source
				+ "' cannot be projected to '" + target
				+ "'; export the schema with --direction all first");

	Json out = input;
	out["direction"] = target;
	if (!out.contains("imports") || !out["imports"].is_array())
		out["imports"] = Json::array();
	if (!out.contains("exports") || !out["exports"].is_array())
		out["exports"] = Json::array();
	if (direction == Direction::Complete)
		return out;
	if (direction == Direction::GuestToHost) {
		out["exports"] = Json::array();
		return out;
	}
	if (direction == Direction::HostToGuest) {
		out["imports"] = Json::array();
		return out;
	}

	// G2G: the consumer imports the producer's exports. The producer's own
	// imports are intentionally discarded.
	const Json exports = input.value("exports", Json::array());
	const std::string producer = input.value("module", "module");
	out["imports"] = Json::array();
	for (Json entry : exports) {
		if (entry.is_object())
			entry["module"] = producer;
		out["imports"].push_back(entry);
	}
	out["exports"] = Json::array();
	return out;
}

std::string emitCHeader(const nlohmann::ordered_json& schema)
{
	return emitCHeaderImpl(schema);
}

std::string emitRustModule(const nlohmann::ordered_json& schema)
{
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "#![allow(non_camel_case_types)]\n";
	out << "#![allow(non_snake_case)]\n\n";
	out << "// Generated from the " << schema.at("schema").get<std::string>()
		<< " " << schema.at("direction").get<std::string>()
		<< " schema. Do not edit.\n\n";
	out << "#[repr(C)]\npub struct wasm_v128 { pub bytes: [u8; 16] }\n";
	for (const std::string& ref : refs)
		out << "#[repr(C)]\npub struct " << ref << " { _private: [u8; 0] }\n";
	out << "\n// Reference types are opaque. Reference parameters are borrowed unless\n"
		   "// the adapter says otherwise; reference results carry an explicit\n"
		   "// ownership note. This module defines no layout and no TypeId.\n\n";

	for (const Json& entry : schema.at("exports")) {
		if (entry.value("kind", "") != "function")
			continue;
		const std::string exportName = entry.at("name").get<std::string>();
		const std::string name = "wasmjit_" + sanitize(exportName);
		out << "// H2G export: " << exportComment(entry) << "\n";
		out << "pub type " << name << "_t = unsafe extern \"C\" fn(";
		emitRustParams(out, entry, signatureResults(entry).size() > 1u);
		out << ")";
		emitRustReturn(out, signatureResults(entry));
		out << ";\n\n";

		// A type alias alone cannot be implemented as a Wasm export. Emit a
		// compilable skeleton under the real export name; the developer fills
		// in the body.
		out << "#[export_name = " << quoteName(exportName, true) << "]\n";
		out << "pub extern \"C\" fn " << name << "_impl(";
		emitRustParams(out, entry, signatureResults(entry).size() > 1u);
		out << ")";
		emitRustReturn(out, signatureResults(entry));
		out << " {\n    todo!("
			<< quoteName("implement " + exportName, true) << ");\n}\n\n";
	}

	// Group imports by Wasm import module: Rust requires the module annotation
	// on the extern block and the field name on each declaration.
	std::map<std::string, std::vector<const Json*>> importsByModule;
	for (const Json& entry : schema.at("imports")) {
		if (entry.value("kind", "") != "function")
			continue;
		importsByModule[importModule(entry)].push_back(&entry);
	}
	for (const auto& [module, entries] : importsByModule) {
		out << "#[link(wasm_import_module = " << quoteName(module, true) << ")]\n";
		out << "extern \"C\" {\n";
		for (const Json* entry : entries) {
			const std::string name = "wasmjit_"
					+ sanitize(module) + "_"
					+ sanitize(entry->at("name").get<std::string>());
			out << "    // G2H import: " << importComment(*entry) << "\n";
			out << "    #[link_name = "
				<< quoteName(entry->at("name").get<std::string>(), true) << "]\n";
			out << "    pub fn " << name << "(";
			emitRustParams(out, *entry,
					signatureResults(*entry).size() > 1u);
			out << ")";
			emitRustReturn(out, signatureResults(*entry));
			out << ";\n";
		}
		out << "}\n\n";
	}
	out << nonFunctionNotes(schema);
	return out.str();
}

namespace {

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

	const auto emitNative = [&](const Json& entry, const std::string& functionName,
								 const char* side) {
		out << "// " << side << ": " << entry.at("name").get<std::string>() << "\n";
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
		emitNative(entry,
				"wasmjit_" + sanitize(entry.at("name").get<std::string>()), "H2G export");
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		emitNative(entry,
				importName(entry),
				"G2H import");
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


std::string pythonType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "ctypes.c_int32";
	if (kind == "i64" || kind == "u64" || kind == "s64")
		return "ctypes.c_int64";
	if (kind == "f32" || kind == "float32") return "ctypes.c_float";
	if (kind == "f64" || kind == "float64") return "ctypes.c_double";
	if (kind == "v128") return "ctypes.c_ubyte * 16";
	if (kind == "void") return "None";
	return "ctypes.c_void_p";
}

std::string emitPythonImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Python ctypes bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "import ctypes\n\n";
	out << "class WasmExternref(ctypes.Structure):\n\tpass\n\n";
	forEachFunction(schema, "imports", [&](const Json& entry) {
		const std::string name = "wasmjit_" + sanitize(importModule(entry)) + "_"
				+ sanitize(entry.at("name").get<std::string>());
		out << "# G2H import: " << importComment(entry) << "\n";
		out << name << "_type = ctypes.CFUNCTYPE("
			<< pythonType(signatureResults(entry).size() == 1
					? signatureResults(entry)[0] : Json{{"type", "void"}});
		for (const Json& param : signatureParams(entry))
			out << ", " << pythonType(param);
		if (signatureResults(entry).size() > 1u)
			out << ", ctypes.c_void_p";
		out << ")\n\n";
	});
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = "wasmjit_" + sanitize(entry.at("name").get<std::string>());
		out << "# H2G export: " << exportComment(entry) << "\n";
		out << name << "_type = ctypes.CFUNCTYPE("
			<< pythonType(signatureResults(entry).size() == 1
					? signatureResults(entry)[0] : Json{{"type", "void"}});
		for (const Json& param : signatureParams(entry))
			out << ", " << pythonType(param);
		if (signatureResults(entry).size() > 1u)
			out << ", ctypes.c_void_p";
		out << ")\n";
		out << name << " = " << name << "_type()\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string emitRubyImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Ruby/Fiddle bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "require 'fiddle'\nrequire 'fiddle/import'\n\n";
	out << "module WasmJitBindings\n\textend Fiddle::Importer\n\n";
	out << "\t# Use dlload with the host's Wasm bridge library before calling.\n";
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "\t# G2H import: " << importComment(entry) << "\n";
		out << "\textern 'void* wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "'\n\n";
	});
	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "\t# H2G export: " << exportComment(entry)
			<< "\n\t# type: Fiddle::TYPE_VOIDP\n\n";
	});
	out << nonFunctionNotes(schema);
	out << "end\n";
	return out.str();
}

std::string luaType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int32_t";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "int64_t";
	if (kind == "f32" || kind == "float32") return "float";
	if (kind == "f64" || kind == "float64") return "double";
	if (kind == "v128") return "uint8_t[16]";
	if (kind == "void") return "void";
	return "void*";
}

std::string emitLuaImpl(const Json& schema)
{
	std::ostringstream out;
	out << "-- Generated LuaJIT FFI bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "local ffi = require(\"ffi\")\n\nffi.cdef[[\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "typedef " << (signatureResults(entry).size() == 1
				? luaType(signatureResults(entry)[0]) : "void")
			<< " (*wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_fn)(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << luaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << luaType(signatureResults(entry).size() == 1
				? signatureResults(entry)[0] : Json{{"type", "void"}})
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << luaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n";
	});
	out << "]]\n\n";
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string swiftType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "Int32";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "Int64";
	if (kind == "f32" || kind == "float32") return "Float";
	if (kind == "f64" || kind == "float64") return "Double";
	if (kind == "void") return "Void";
	return "UnsafeMutableRawPointer?";
}

std::string emitSwiftImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated Swift/C-convention bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "import Foundation\n\n";
	out << "public typealias WasmExternref = UnsafeMutableRawPointer\n\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "public typealias Wasmjit_" << name << "_Fn = @convention(c) (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << swiftType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "UnsafeMutableRawPointer?";
		out << ") -> "
			<< (signatureResults(entry).size() == 1
					? swiftType(signatureResults(entry)[0]) : "Void")
			<< "\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << "@_silgen_name(\"" << entry.at("name").get<std::string>() << "\")\n";
		out << "public func wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << swiftType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "result: UnsafeMutableRawPointer";
		out << ") -> "
			<< (signatureResults(entry).size() == 1
					? swiftType(signatureResults(entry)[0]) : "Void") << "\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string dType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "long";
	if (kind == "f32" || kind == "float32") return "float";
	if (kind == "f64" || kind == "float64") return "double";
	if (kind == "v128") return "ubyte[16]";
	if (kind == "void") return "void";
	return "void*";
}

std::string emitDLangImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated D extern(C) bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "extern(C):\n\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "alias Wasmjit_" << name << "_Fn = " << dType(
				signatureResults(entry).size() == 1
						? signatureResults(entry)[0] : Json{{"type", "void"}})
			<< " function(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << dType(signatureResults(entry).size() == 1
				? signatureResults(entry)[0] : Json{{"type", "void"}})
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string nimType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int32";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "int64";
	if (kind == "f32" || kind == "float32") return "float32";
	if (kind == "f64" || kind == "float64") return "float64";
	if (kind == "v128") return "array[16, uint8]";
	if (kind == "void") return "void";
	return "pointer";
}

std::string emitNimImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Nim bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "type Wasmjit_" << name << "_Fn = proc(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << nimType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? nimType(signatureResults(entry)[0]) : "void")
			<< " {.cdecl.}\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "proc wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << nimType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? nimType(signatureResults(entry)[0]) : "void")
			<< " {.importc: \"" << entry.at("name").get<std::string>()
			<< "\", cdecl.}\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string crystalType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "Int32";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "Int64";
	if (kind == "f32" || kind == "float32") return "Float32";
	if (kind == "f64" || kind == "float64") return "Float64";
	if (kind == "v128") return "StaticArray(UInt8, 16)";
	if (kind == "void") return "Void";
	return "Void*";
}

std::string emitCrystalImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Crystal lib bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "lib WasmJitBindings\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "  alias Wasmjit_" << name << "_Fn = (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << crystalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "Void*";
		out << ") -> " << (signatureResults(entry).size() == 1
				? crystalType(signatureResults(entry)[0]) : "Void") << "\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "  # G2H import: " << importComment(entry) << "\n";
		out << "  fun wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << " : " << crystalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "result : Void*";
		out << ") : " << (signatureResults(entry).size() == 1
				? crystalType(signatureResults(entry)[0]) : "Void") << "\n";
	});
	out << "end\n";
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string adaType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "Interfaces.Integer_32";
	if (kind == "i64" || kind == "u64" || kind == "s64")
		return "Interfaces.Integer_64";
	if (kind == "f32" || kind == "float32") return "Interfaces.Float_32";
	if (kind == "f64" || kind == "float64") return "Interfaces.Float_64";
	if (kind == "void") return "Ada.Numerics.Void";
	return "System.Address";
}

std::string emitAdaImpl(const Json& schema)
{
	std::ostringstream out;
	out << "-- Generated Ada bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "with Interfaces; use Interfaces;\n";
	out << "with System;\n\npackage WasmJit.Bindings is\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "   type Wasmjit_" << name << "_Fn is access function (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << pascalName(argumentName(entry, i)) << " : " << adaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "Result : System.Address";
		out << ") return " << (signatureResults(entry).size() == 1
				? adaType(signatureResults(entry)[0]) : "System.Address")
			<< " with Convention => C;\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "   -- G2H import: " << importComment(entry) << "\n";
		out << "   function Wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << " (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << pascalName(argumentName(entry, i)) << " : " << adaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "Result : System.Address";
		out << ") return " << (signatureResults(entry).size() == 1
				? adaType(signatureResults(entry)[0]) : "System.Address")
			<< " with Import, Convention => C, External_Name => \""
			<< entry.at("name").get<std::string>() << "\";\n";
	});
	out << "end WasmJit.Bindings;\n";
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string pascalType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "LongInt";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "Int64";
	if (kind == "f32" || kind == "float32") return "Single";
	if (kind == "f64" || kind == "float64") return "Double";
	if (kind == "v128") return "array[0..15] of Byte";
	if (kind == "void") return "";
	return "Pointer";
}

std::string emitPascalImpl(const Json& schema)
{
	std::ostringstream out;
	out << "{ Generated Pascal bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit. }\n";
	out << "unit WasmJitBindings;\n\ninterface\n\ntype\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "  Wasmjit_" << name << "_Fn = function (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << pascalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: Pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? pascalType(signatureResults(entry)[0]) : "LongInt")
			<< "; cdecl;\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "  // G2H import: " << importComment(entry) << "\n";
		out << "  function wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << pascalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: Pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? pascalType(signatureResults(entry)[0]) : "LongInt")
			<< "; cdecl; external '" << importModule(entry) << "' name '"
			<< entry.at("name").get<std::string>() << "';\n";
	});
	out << "\nimplementation\n\nend.\n";
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string haskellType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "CInt";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "CLLong";
	if (kind == "f32" || kind == "float32") return "CFloat";
	if (kind == "f64" || kind == "float64") return "CDouble";
	if (kind == "void") return "()";
	return "Ptr ()";
}

std::string emitHaskellImpl(const Json& schema)
{
	std::ostringstream out;
	out << "-- Generated Haskell FFI bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "{-# LANGUAGE ForeignFunctionInterface #-}\n";
	out << "module WasmJitBindings where\n\n";
	out << "import Foreign.C.Types\nimport Foreign.Ptr\n\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "-- H2G export: " << exportComment(entry) << "\n";
		out << "type Wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_Fn = ";
		const Json& params = signatureParams(entry);
		for (const Json& param : params)
			out << haskellType(param) << " -> ";
		if (signatureResults(entry).size() > 1u)
			out << "Ptr () -> ";
		out << "IO " << (signatureResults(entry).size() == 1
				? haskellType(signatureResults(entry)[0]) : "()") << "\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "foreign import ccall \"" << entry.at("name").get<std::string>()
			<< "\" wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "\n  :: ";
		for (const Json& param : signatureParams(entry))
			out << haskellType(param) << " -> ";
		if (signatureResults(entry).size() > 1u)
			out << "Ptr () -> ";
		out << "IO " << (signatureResults(entry).size() == 1
				? haskellType(signatureResults(entry)[0]) : "()") << "\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


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
	if (direction != "h2g")
		return "// cpp-typedefs is an H2G (host calls guest) surface; no output "
			   "for " + direction + ".\n";

	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);
	const bool emitImports = false;
	const bool emitExports = true;
	std::ostringstream out;
	out << "// Generated C++ binding surface from the "
		<< schema.at("schema").get<std::string>() << " " << direction
		<< " schema. Do not edit.\n";
	out << "#ifndef WASMJIT_GENERATED_" << sanitize(schema.value("module", "module"))
		<< "_CALLABLES_HPP\n#define WASMJIT_GENERATED_"
		<< sanitize(schema.value("module", "module")) << "_CALLABLES_HPP\n\n";
	out << "#include <cstdint>\n\n";
	out << "namespace WASM { struct VMContext; struct ModuleInstance; }\n";
	out << "typedef struct wasm_v128 { uint8_t bytes[16]; } wasm_v128;\n";
	for (const std::string& ref : refs)
		out << "struct " << ref << ";\n";
	out << "\n// A Callable's fnPtr is `ret f(VMContext*, params...)`; multi-value\n"
		   "// results use a trailing result buffer.\n";
	if (emitImports) {
		out << "\n// Host imports the guest expects you to implement (G2H).\n";
		forEachFunction(schema, "imports", [&](const Json& entry) {
			emitCppImportDeclaration(out, entry);
		});
	}
	if (emitExports) {
		out << "\n// Guest exports the host can resolve (H2G).\n";
		forEachFunction(schema, "exports", [&](const Json& entry) {
			out << "// export: " << exportComment(entry) << "\n";
			emitCppExportDeclaration(out, entry);
		});
		out << "\n// Resolve the pointers above from a loaded module instance.\n";
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
	if (direction != "h2g")
		return "// cpp-source is an H2G (host calls guest) surface; no output "
			   "for " + direction + ".\n";

	const bool emitImports = false;
	const bool emitExports = true;
	const std::string module = sanitize(schema.value("module", "module"));
	std::ostringstream out;
	out << "// Generated C++ binding implementation from the "
		<< schema.at("schema").get<std::string>() << " " << direction
		<< " schema. Do not edit.\n";
	out << "#include \"wasmjit_" << module << "_callables.hpp\"\n";
	if (emitExports)
		out << "#include <WasmBase/WasmModuleInstance.hpp>\n";
	out << "\n";
	if (emitImports) {
		forEachFunction(schema, "imports", [&](const Json& entry) {
			emitCppImportStub(out, entry);
		});
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
	out << "typedef struct wasm_v128 { uint8_t bytes[16]; } wasm_v128;\n";
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

} // namespace

const char* languageName(Language language) noexcept
{
	switch (language) {
	case Language::CHeader: return "c-header";
	case Language::Rust: return "rust";
	case Language::TypeScript: return "typescript";
	case Language::AssemblyScript: return "assemblyscript";
	case Language::Zig: return "zig";
	case Language::Kotlin: return "kotlin";
	case Language::MoonBit: return "moonbit";
	case Language::Go: return "go";
	case Language::Dart: return "dart";
	case Language::Java: return "java";
	case Language::CSharp: return "csharp";
	case Language::FSharp: return "fsharp";
	case Language::Python: return "python";
	case Language::Ruby: return "ruby";
	case Language::Lua: return "lua";
	case Language::Swift: return "swift";
	case Language::DLang: return "d";
	case Language::Nim: return "nim";
	case Language::Crystal: return "crystal";
	case Language::Ada: return "ada";
	case Language::Pascal: return "pascal";
	case Language::Haskell: return "haskell";
	case Language::CppAdapter: return "cpp-adapter";
	case Language::CppTypedefs: return "cpp-typedefs";
	case Language::CppSource: return "cpp-source";
	}
	return "c-header";
}

bool parseLanguage(std::string_view name, Language& language) noexcept
{
	for (const Language candidate : { Language::CHeader, Language::Rust,
				 Language::TypeScript, Language::AssemblyScript, Language::Zig,
				 Language::Kotlin, Language::MoonBit, Language::Go, Language::Dart,
				 Language::Java, Language::CSharp, Language::FSharp,
				 Language::Python, Language::Ruby, Language::Lua,
				 Language::Swift, Language::DLang, Language::Nim,
				 Language::Crystal, Language::Ada, Language::Pascal,
				 Language::Haskell, Language::CppAdapter,
				 Language::CppTypedefs, Language::CppSource }) {
		if (name == languageName(candidate)) {
			language = candidate;
			return true;
		}
	}
	return false;
}

std::string emit(const nlohmann::ordered_json& schema, Language language)
{
	switch (language) {
	case Language::CHeader: return emitCHeader(schema);
	case Language::Rust: return emitRustModule(schema);
	case Language::TypeScript: return emitTypeScriptImpl(schema);
	case Language::AssemblyScript: return emitAssemblyScriptImpl(schema);
	case Language::Zig: return emitZigImpl(schema);
	case Language::Kotlin: return emitKotlinImpl(schema);
	case Language::MoonBit: return emitMoonBitImpl(schema);
	case Language::Go: return emitGoImpl(schema);
	case Language::Dart: return emitDartImpl(schema);
	case Language::Java: return emitJavaImpl(schema);
	case Language::CSharp: return emitCSharpImpl(schema);
	case Language::FSharp: return emitFSharpImpl(schema);
	case Language::Python: return emitPythonImpl(schema);
	case Language::Ruby: return emitRubyImpl(schema);
	case Language::Lua: return emitLuaImpl(schema);
	case Language::Swift: return emitSwiftImpl(schema);
	case Language::DLang: return emitDLangImpl(schema);
	case Language::Nim: return emitNimImpl(schema);
	case Language::Crystal: return emitCrystalImpl(schema);
	case Language::Ada: return emitAdaImpl(schema);
	case Language::Pascal: return emitPascalImpl(schema);
	case Language::Haskell: return emitHaskellImpl(schema);
	case Language::CppAdapter: return emitCppAdapterImpl(schema);
	case Language::CppTypedefs: return emitCppTypedefsImpl(schema);
	case Language::CppSource: return emitCppSourceImpl(schema);
	}
	return {};
}

} // namespace WASM::Bindings
