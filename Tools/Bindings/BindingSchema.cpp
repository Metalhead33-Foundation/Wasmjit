#include "BindingSchema.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <set>
#include <sstream>
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

Json descriptorJson(const WASM::ExternTypeDescriptor& type, const WASM::TypeRegistry& types)
{
	Json out;
	switch (type.kind) {
	case WASM::ExternalKind::Function:
		out["kind"] = "function";
		out["type"] = typeJson(types.canonicalType(type.typeId), types);
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
	for (const WASM::ImportDescriptor& descriptor : module.importDescriptors()) {
		Json import = descriptorJson(descriptor.type, types);
		import["module"] = descriptor.moduleName;
		import["name"] = descriptor.fieldName;
		schema["imports"].push_back(import);
	}
	schema["exports"] = Json::array();
	for (const WASM::ExportDescriptor& descriptor : module.exportDescriptors()) {
		Json exportEntry = descriptorJson(descriptor.type, types);
		exportEntry["name"] = descriptor.name;
		schema["exports"].push_back(exportEntry);
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
	if (kind == "i32") return "int32_t";
	if (kind == "i64") return "int64_t";
	if (kind == "f32") return "float";
	if (kind == "f64") return "double";
	if (kind == "v128") return "wasm_v128";
	if (kind == "void") return "void";
	if (isReference(type))
		return refTypeName(type) + "*";
	return "void";
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

void emitFunction(std::ostringstream& out, const Json& entry, const std::string& name,
		const char* side)
{
	const Json& type = entry.at("type");
	std::string display = entry.value("module", "");
	if (!display.empty())
		display += ".";
	display += entry.at("name").get<std::string>();

	out << "/* " << side << ": " << display << " */\n";
	const Json& results = type.at("results");
	const bool multiValue = results.size() > 1u;
	if (multiValue) {
		out << "/* multi-value result: use a generated result buffer */\n";
		out << "void " << name << "(";
	} else if (results.empty()) {
		out << "void " << name << "(";
	} else {
		out << cType(results[0]) << " " << name << "(";
	}

	const Json& params = type.at("params");
	if (params.empty()) {
		if (!multiValue)
			out << "void";
	} else {
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0)
				out << ", ";
			out << cType(params[i]) << " arg" << i;
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


std::string emitCHeaderImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

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
				"G2H export");
	}
	for (const Json& entry : schema.at("imports")) {
		if (entry.value("kind", "") != "function")
			continue;
		emitFunction(out, entry,
				"wasmjit_" + sanitize(entry.at("module").get<std::string>()) + "_"
						+ sanitize(entry.at("name").get<std::string>()),
				"H2G import");
	}

	out << "#ifdef __cplusplus\n}\n#endif\n\n";
	out << "#endif /* " << guard << " */\n";
	return out.str();
}

std::string rustType(const Json& type)
{
	if (type.value("packed", false))
		return type.value("type", "") == "i8" ? "i8" : "i16";
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "i32";
	if (kind == "i64") return "i64";
	if (kind == "f32") return "f32";
	if (kind == "f64") return "f64";
	if (kind == "v128") return "wasm_v128";
	if (kind == "void") return "()";
	if (isReference(type))
		return "*mut " + refTypeName(type);
	return "()";
}

void emitRustParams(std::ostringstream& out, const Json& params, bool resultBuffer)
{
	bool first = true;
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (!first)
			out << ", ";
		first = false;
		out << "arg" << i << ": " << rustType(params[i]);
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
	if (kind == "i32" || kind == "i64" || kind == "f32" || kind == "f64")
		return "number";
	if (kind == "v128") return "Uint8Array";
	if (kind == "void") return "void";
	if (isReference(type)) return refPascal(type);
	return "unknown";
}

std::string asType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "i32";
	if (kind == "i64") return "i64";
	if (kind == "f32") return "f32";
	if (kind == "f64") return "f64";
	if (kind == "v128") return "v128";
	if (kind == "void") return "void";
	if (isReference(type)) return "externref";
	return "i32";
}

std::string zigType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "i32";
	if (kind == "i64") return "i64";
	if (kind == "f32") return "f32";
	if (kind == "f64") return "f64";
	if (kind == "v128") return "wasm_v128";
	if (kind == "void") return "void";
	if (isReference(type)) return "*" + refTypeName(type);
	return "void";
}

std::string kotlinType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "Int";
	if (kind == "i64") return "Long";
	if (kind == "f32") return "Float";
	if (kind == "f64") return "Double";
	if (kind == "v128") return "WasmV128";
	if (kind == "void") return "Unit";
	if (isReference(type)) return refPascal(type);
	return "Unit";
}

std::string moonbitType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "Int";
	if (kind == "i64") return "Int64";
	if (kind == "f32") return "Float";
	if (kind == "f64") return "Double";
	if (kind == "v128") return "Bytes";
	if (kind == "void") return "Unit";
	if (isReference(type)) return refPascal(type);
	return "Unit";
}

std::string goType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "int32";
	if (kind == "i64") return "int64";
	if (kind == "f32") return "float32";
	if (kind == "f64") return "float64";
	if (kind == "v128") return "[16]byte";
	if (kind == "void") return "";
	// Go's wasmimport/wasmexport accept integer handles, not pointer types
	// (docs/EMBEDDING_SURFACE.md §12.6: prefer a table handle).
	if (isReference(type)) return "uintptr";
	return "";
}

std::string dartType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "i64") return "int";
	if (kind == "f32" || kind == "f64") return "double";
	if (kind == "v128") return "Pointer<Uint8>";
	if (kind == "void") return "void";
	if (isReference(type)) return "Pointer<Void>";
	return "void";
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

} // namespace

const char* directionName(Direction direction) noexcept
{
	switch (direction) {
	case Direction::GuestToHost: return "g2h";
	case Direction::HostToGuest: return "h2g";
	case Direction::GuestToGuest: return "g2g";
	}
	return "g2h";
}

Json moduleSchema(const WASM::Module& module, const WASM::TypeRegistry& types,
		Direction direction, std::string_view moduleName)
{
	return moduleSchemaImpl(module, types, direction, moduleName);
}

std::string emitCHeader(const nlohmann::ordered_json& schema)
{
	return emitCHeaderImpl(schema);
}

std::string emitRustModule(const nlohmann::ordered_json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

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
		const Json& type = entry.at("type");
		const std::string name = "wasmjit_" + sanitize(entry.at("name").get<std::string>());
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "pub type " << name << "_t = unsafe extern \"C\" fn(";
		emitRustParams(out, type.at("params"), type.at("results").size() > 1u);
		out << ")";
		emitRustReturn(out, type.at("results"));
		out << ";\n\n";
	}

	if (!schema.at("imports").empty()) {
		out << "extern \"C\" {\n";
		for (const Json& entry : schema.at("imports")) {
			if (entry.value("kind", "") != "function")
				continue;
			const Json& type = entry.at("type");
			const std::string name = "wasmjit_"
					+ sanitize(entry.at("module").get<std::string>()) + "_"
					+ sanitize(entry.at("name").get<std::string>());
			out << "    // H2G import: " << entry.at("module").get<std::string>() << "."
				<< entry.at("name").get<std::string>() << "\n";
			out << "    pub fn " << name << "(";
			emitRustParams(out, type.at("params"), type.at("results").size() > 1u);
			out << ")";
			emitRustReturn(out, type.at("results"));
			out << ";\n";
		}
		out << "}\n";
	}
	return out.str();
}

namespace {

std::string emitTypeScriptImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

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
		const Json& type = entry.at("type");
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << tsType(params[i]);
			if (isReference(params[i])) out << " /* borrowed */";
		}
		if (params.empty()) out << "void";
		out << ")";
		const Json& results = type.at("results");
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
		const Json& type = entry.at("type");
		out << "// H2G import: " << entry.at("module").get<std::string>() << "."
			<< entry.at("name").get<std::string>() << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("module").get<std::string>()) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << tsType(params[i]);
			if (isReference(params[i])) out << " /* borrowed */";
		}
		if (params.empty()) out << "void";
		out << ")";
		const Json& results = type.at("results");
		if (results.size() == 1)
			out << ": " << tsType(results[0]);
		else if (results.size() > 1)
			out << ": void";
		else
			out << ": void";
		out << ";\n\n";
	});
	return out.str();
}

std::string emitAssemblyScriptImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";

	forEachFunction(schema, "imports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "@external(\"" << entry.at("module").get<std::string>() << "\", \""
			<< entry.at("name").get<std::string>() << "\")\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("module").get<std::string>()) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << asType(params[i]);
		}
		out << ")";
		const Json& results = type.at("results");
		out << (results.size() == 1 ? ": " + asType(results[0]) : ": void");
		out << ";\n\n";
	});

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << asType(params[i]);
		}
		out << ")";
		const Json& results = type.at("results");
		out << (results.size() == 1 ? ": " + asType(results[0]) : ": void");
		out << ";\n\n";
	});
	return out.str();
}

std::string emitZigImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";
	out << "pub const wasm_v128 = [16]u8;\n";
	for (const std::string& ref : refs)
		out << "pub const " << ref << " = opaque {};\n";
	out << "\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "pub const wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_t = *const fn (";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << zigType(params[i]);
		}
		const bool multi = type.at("results").size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result: *anyopaque";
		}
		out << ") callconv(.c) ";
		if (multi || type.at("results").empty())
			out << "void";
		else
			out << zigType(type.at("results")[0]);
		out << ";\n\n";
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "// H2G import: " << entry.at("module").get<std::string>() << "."
			<< entry.at("name").get<std::string>() << "\n";
		out << "pub extern \"" << entry.at("module").get<std::string>() << "\" fn wasmjit_"
			<< sanitize(entry.at("module").get<std::string>()) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << zigType(params[i]);
		}
		const bool multi = type.at("results").size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result: *anyopaque";
		}
		out << ") callconv(.c) ";
		if (multi || type.at("results").empty())
			out << "void";
		else
			out << zigType(type.at("results")[0]);
		out << ";\n\n";
	});
	return out.str();
}

std::string emitKotlinImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";
	out << "class WasmV128(val bytes: ByteArray)\n";
	for (const std::string& ref : refs)
		out << "class " << pascalName(ref) << " private constructor()\n";
	out << "\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "typealias wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_t = (";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << kotlinType(params[i]);
		}
		out << ") -> "
			<< (type.at("results").size() == 1 ? kotlinType(type.at("results")[0])
											   : "Unit");
		out << "\n\n";
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "// H2G import: " << entry.at("module").get<std::string>() << "."
			<< entry.at("name").get<std::string>() << "\n";
		out << "external fun wasmjit_"
			<< sanitize(entry.at("module").get<std::string>()) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << kotlinType(params[i]);
		}
		out << "): "
			<< (type.at("results").size() == 1 ? kotlinType(type.at("results")[0])
											   : "Unit");
		out << "\n\n";
	});
	return out.str();
}

std::string emitMoonBitImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

	std::ostringstream out;
	out << "// Generated from the " << schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n\n";
	for (const std::string& ref : refs)
		out << "#external\ntype " << pascalName(ref) << "\n";
	out << "\n";

	forEachFunction(schema, "imports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "// H2G import: " << entry.at("module").get<std::string>() << "."
			<< entry.at("name").get<std::string>() << "\n";
		std::string borrow;
		for (std::size_t i = 0; i < type.at("params").size(); ++i) {
			if (isReference(type.at("params")[i])) {
				if (!borrow.empty()) borrow += ", ";
				borrow += "arg" + std::to_string(i);
			}
		}
		if (!borrow.empty())
			out << "#borrow(" << borrow << ")\n";
		out << "extern \"wasm\" fn wasmjit_"
			<< sanitize(entry.at("module").get<std::string>()) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		for (std::size_t i = 0; i < type.at("params").size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << " : " << moonbitType(type.at("params")[i]);
		}
		out << ") -> "
			<< (type.at("results").size() == 1 ? moonbitType(type.at("results")[0])
											   : "Unit");
		out << " = \"" << entry.at("module").get<std::string>() << "\" \""
			<< entry.at("name").get<std::string>() << "\"\n\n";
	});

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "// G2H export: " << entry.at("name").get<std::string>() << "(";
		for (std::size_t i = 0; i < type.at("params").size(); ++i) {
			if (i != 0) out << ", ";
			out << moonbitType(type.at("params")[i]);
		}
		out << ") -> "
			<< (type.at("results").size() == 1 ? moonbitType(type.at("results")[0])
											   : "Unit")
			<< "\n";
	});
	return out.str();
}

std::string emitGoImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

	std::ostringstream out;
	out << "package wasmjitbindings\n\n";
	for (const std::string& ref : refs)
		out << "type " << pascalName(ref) << " struct{ _ [0]byte }\n";
	out << "\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "//go:wasmexport " << entry.at("name").get<std::string>() << "\n";
		out << "func wasmjit_" << sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << " " << goType(params[i]);
		}
		const bool multi = type.at("results").size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result *byte";
		}
		out << ")";
		if (!multi && type.at("results").size() == 1)
			out << " " << goType(type.at("results")[0]);
		out << " { panic(\"generated adapter stub\") }\n\n";
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		const Json& type = entry.at("type");
		out << "//go:wasmimport " << entry.at("module").get<std::string>() << " "
			<< entry.at("name").get<std::string>() << "\n";
		out << "func wasmjit_" << sanitize(entry.at("module").get<std::string>()) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = type.at("params");
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << " " << goType(params[i]);
		}
		const bool multi = type.at("results").size() > 1u;
		if (multi) {
			if (!params.empty()) out << ", ";
			out << "result *byte";
		}
		out << ")";
		if (!multi && type.at("results").size() == 1)
			out << " " << goType(type.at("results")[0]);
		out << "\n\n";
	});
	return out.str();
}

std::string dartNativeType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "Int32";
	if (kind == "i64") return "Int64";
	if (kind == "f32") return "Float";
	if (kind == "f64") return "Double";
	if (kind == "v128") return "Pointer<Uint8>";
	if (kind == "void") return "Void";
	if (isReference(type)) return "Pointer<Void>";
	return "Void";
}

std::string emitDartImpl(const Json& schema)
{
	std::set<std::string> refs;
	collectRefs(schema.at("types"), refs);
	collectRefs(schema.at("imports"), refs);
	collectRefs(schema.at("exports"), refs);

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
		const Json& type = entry.at("type");
		out << "// " << side << ": " << entry.value("module", "");
		if (!entry.value("module", "").empty()) out << ".";
		out << entry.at("name").get<std::string>() << "\n";
		out << "@Native<" << dartNativeType(type.at("results").size() == 1
												   ? type.at("results")[0]
												   : Json{{"type", "void"}})
			<< " Function(";
		for (std::size_t i = 0; i < type.at("params").size(); ++i) {
			if (i != 0) out << ", ";
			out << dartNativeType(type.at("params")[i]);
		}
		out << ")>(" << "symbol: '" << functionName << "')\n";
		out << "external "
			<< (type.at("results").size() == 1 ? dartType(type.at("results")[0]) : "void")
			<< " " << functionName << "(";
		for (std::size_t i = 0; i < type.at("params").size(); ++i) {
			if (i != 0) out << ", ";
			out << dartType(type.at("params")[i]) << " arg" << i;
		}
		out << ");\n\n";
	};

	forEachFunction(schema, "exports", [&](const Json& entry) {
		emitNative(entry,
				"wasmjit_" + sanitize(entry.at("name").get<std::string>()), "G2H export");
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		emitNative(entry,
				"wasmjit_" + sanitize(entry.at("module").get<std::string>()) + "_"
						+ sanitize(entry.at("name").get<std::string>()),
				"H2G import");
	});
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
	}
	return "c-header";
}

bool parseLanguage(std::string_view name, Language& language) noexcept
{
	for (const Language candidate : { Language::CHeader, Language::Rust,
				 Language::TypeScript, Language::AssemblyScript, Language::Zig,
				 Language::Kotlin, Language::MoonBit, Language::Go, Language::Dart }) {
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
	}
	return {};
}

} // namespace WASM::Bindings
