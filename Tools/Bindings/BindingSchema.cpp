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

std::string importComment(const Json& entry)
{
	return importModule(entry) + "." + entry.at("name").get<std::string>();
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
		const char* side)
{
	std::string display = entry.value("module", "");
	if (!display.empty())
		display += ".";
	display += entry.at("name").get<std::string>();

	out << "/* " << side << ": " << display << " */\n";
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
				importName(entry),
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
	return out;
}

Json componentImportJson(const WASM::ImportDecl& importDecl,
		const WASM::Component& component, const WASM::TypeStore& store)
{
	Json out;
	out["name"] = importDecl.name.name;
	fillComponentExtern(out, importDecl.type, component, store, 0);
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
	for (const WASM::ImportDecl& importDecl : component.imports)
		schema["imports"].push_back(componentImportJson(importDecl, component, store));
	schema["exports"] = Json::array();
	for (const WASM::ComponentExport& exportDecl : component.exports)
		schema["exports"].push_back(
				componentExportJson(exportDecl, component, spaces, store));
	return schema;
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
		const std::string name = "wasmjit_" + sanitize(entry.at("name").get<std::string>());
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "pub type " << name << "_t = unsafe extern \"C\" fn(";
		emitRustParams(out, signatureParams(entry), signatureResults(entry).size() > 1u);
		out << ")";
		emitRustReturn(out, signatureResults(entry));
		out << ";\n\n";
	}

	if (!schema.at("imports").empty()) {
		out << "extern \"C\" {\n";
		for (const Json& entry : schema.at("imports")) {
			if (entry.value("kind", "") != "function")
				continue;
			const std::string name = "wasmjit_"
					+ sanitize(importModule(entry)) + "_"
					+ sanitize(entry.at("name").get<std::string>());
			out << "    // H2G import: " << importComment(entry) << "\n";
			out << "    pub fn " << name << "(";
			emitRustParams(out, signatureParams(entry), signatureResults(entry).size() > 1u);
			out << ")";
			emitRustReturn(out, signatureResults(entry));
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
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << tsType(params[i]);
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
		out << "// H2G import: " << importComment(entry) << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << tsType(params[i]);
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
			out << "arg" << i << ": " << asType(params[i]);
		}
		out << ")";
		const Json& results = signatureResults(entry);
		out << (results.size() == 1 ? ": " + asType(results[0]) : ": void");
		out << ";\n\n";
	});

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "export declare function wasmjit_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << asType(params[i]);
		}
		out << ")";
		const Json& results = signatureResults(entry);
		out << (results.size() == 1 ? ": " + asType(results[0]) : ": void");
		out << ";\n\n";
	});
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
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
		out << "pub const wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_t = *const fn (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << zigType(params[i]);
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
	});

	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// H2G import: " << importComment(entry) << "\n";
		out << "pub extern \"" << importModule(entry) << "\" fn wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << zigType(params[i]);
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
	});
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
		out << "// G2H export: " << entry.at("name").get<std::string>() << "\n";
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
		out << "// H2G import: " << importComment(entry) << "\n";
		out << "external fun wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << ": " << kotlinType(params[i]);
		}
		out << "): "
			<< (signatureResults(entry).size() == 1 ? kotlinType(signatureResults(entry)[0])
											   : "Unit");
		out << "\n\n";
	});
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
		out << "// H2G import: " << importComment(entry) << "\n";
		std::string borrow;
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (isReference(signatureParams(entry)[i])) {
				if (!borrow.empty()) borrow += ", ";
				borrow += "arg" + std::to_string(i);
			}
		}
		if (!borrow.empty())
			out << "#borrow(" << borrow << ")\n";
		out << "extern \"wasm\" fn wasmjit_"
			<< sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (i != 0) out << ", ";
			out << "arg" << i << " : " << moonbitType(signatureParams(entry)[i]);
		}
		out << ") -> "
			<< (signatureResults(entry).size() == 1 ? moonbitType(signatureResults(entry)[0])
											   : "Unit");
		out << " = \"" << importModule(entry) << "\" \""
			<< entry.at("name").get<std::string>() << "\"\n\n";
	});

	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "// G2H export: " << entry.at("name").get<std::string>() << "(";
		for (std::size_t i = 0; i < signatureParams(entry).size(); ++i) {
			if (i != 0) out << ", ";
			out << moonbitType(signatureParams(entry)[i]);
		}
		out << ") -> "
			<< (signatureResults(entry).size() == 1 ? moonbitType(signatureResults(entry)[0])
											   : "Unit")
			<< "\n";
	});
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
			out << "arg" << i << " " << goType(params[i]);
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
			out << "arg" << i << " " << goType(params[i]);
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
			out << dartType(signatureParams(entry)[i]) << " arg" << i;
		}
		out << ");\n\n";
	};

	forEachFunction(schema, "exports", [&](const Json& entry) {
		emitNative(entry,
				"wasmjit_" + sanitize(entry.at("name").get<std::string>()), "G2H export");
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		emitNative(entry,
				importName(entry),
				"H2G import");
	});
	return out.str();
}

std::string emitCppAdapterImpl(const Json& schema)
{
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
	out << "}\n";
	out << "} // namespace\n\n";
	out << "extern \"C\" {\n";

	forEachFunction(schema, "exports", [&](const Json& entry) {
		const Json& params = signatureParams(entry);
		const Json& results = signatureResults(entry);
		const std::string exportName = entry.at("name").get<std::string>();
		const std::string name = "wasmjit_" + sanitize(exportName);
		out << "\n// G2H export: " << exportName << "\n";
		if (results.size() > 1u) {
			out << "// multi-value export: use WASM::callCallableWithResults in the adapter\n";
			return;
		}
		const std::string ret = results.size() == 1 ? cType(results[0]) : "void";
		out << ret << " " << name << "(";
		if (params.empty())
			out << "void";
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << cType(params[i]) << " arg" << i;
		}
		out << ")\n{\n";
		std::string callArgs;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) callArgs += ", ";
			if (isReference(params[i]))
				callArgs += "reinterpret_cast<void*>(arg" + std::to_string(i) + ")";
			else
				callArgs += "arg" + std::to_string(i);
		}
		if (results.empty()) {
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
	case Language::CppAdapter: return "cpp-adapter";
	}
	return "c-header";
}

bool parseLanguage(std::string_view name, Language& language) noexcept
{
	for (const Language candidate : { Language::CHeader, Language::Rust,
				 Language::TypeScript, Language::AssemblyScript, Language::Zig,
				 Language::Kotlin, Language::MoonBit, Language::Go, Language::Dart,
				 Language::CppAdapter }) {
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
	case Language::CppAdapter: return emitCppAdapterImpl(schema);
	}
	return {};
}

} // namespace WASM::Bindings
