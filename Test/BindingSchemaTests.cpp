#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "helper.hpp"
#include "TestStore.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmTypeRegistry.hpp"
#include "WasmBase/WasmTypeStore.hpp"

// docs/EMBEDDING_SURFACE.md §12.8: a test-only first slice of TB-1/TB-2/TB-8.
// It projects core module descriptors into the neutral binding schema and emits
// a C header from it. Nothing here is an engine API; the schema is the contract.
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

Json heapJson(const WASM::CanonHeapType& heap)
{
	Json out;
	switch (heap.kind) {
	case WASM::CanonHeapType::Kind::Abstract:
		out["heap"] = abstractHeapName(heap.abstract);
		break;
	case WASM::CanonHeapType::Kind::Global:
		out["heap"] = "defined";
		out["typeId"] = heap.id.value;
		switch (testStore().types().canonicalType(heap.id).kind()) {
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

Json valueTypeJson(const WASM::CanonValueType& type)
{
	if (type.opcode == WASM::ValueTypeCode::Ref || type.opcode == WASM::ValueTypeCode::RefNull) {
		Json out = heapJson(type.heap);
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

Json storageTypeJson(const WASM::CanonStorageType& storage)
{
	if (storage.isPacked) {
		Json out;
		out["type"] = valueTypeName(storage.packed);
		out["packed"] = true;
		return out;
	}
	return valueTypeJson(storage.value);
}

Json fieldTypeJson(const WASM::CanonFieldType& field)
{
	Json out;
	out["type"] = storageTypeJson(field.storage);
	out["mutable"] = field.isMutable;
	return out;
}

Json typeJson(const WASM::CoreTypeView& view)
{
	Json out;
	switch (view.kind()) {
	case WASM::CanonTypeKind::Func:
		out["kind"] = "func";
		out["params"] = Json::array();
		for (const WASM::CanonStorageType& param : view.funcParams())
			out["params"].push_back(storageTypeJson(param));
		out["results"] = Json::array();
		for (const WASM::CanonStorageType& result : view.funcResults())
			out["results"].push_back(storageTypeJson(result));
		break;
	case WASM::CanonTypeKind::Struct:
		out["kind"] = "struct";
		out["fields"] = Json::array();
		for (const WASM::CanonFieldType& field : view.structFields())
			out["fields"].push_back(fieldTypeJson(field));
		break;
	case WASM::CanonTypeKind::Array:
		out["kind"] = "array";
		out["element"] = fieldTypeJson(view.arrayElement());
		break;
	case WASM::CanonTypeKind::Cont:
		out["kind"] = "cont";
		out["func"] = heapJson(view.contFunc());
		break;
	case WASM::CanonTypeKind::Import:
		out["kind"] = "import";
		out["bound"] = heapJson(view.importBound());
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

Json descriptorJson(const WASM::ExternTypeDescriptor& type)
{
	Json out;
	switch (type.kind) {
	case WASM::ExternalKind::Function:
		out["kind"] = "function";
		out["type"] = typeJson(testStore().types().canonicalType(type.typeId));
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
			out["type"] = typeJson(testStore().types().canonicalType(type.typeId));
		break;
	case WASM::ExternalKind::Type:
		out["kind"] = "type";
		if (!type.typeId.isNone())
			out["type"] = typeJson(testStore().types().canonicalType(type.typeId));
		break;
	}
	if (type.isExact)
		out["exact"] = true;
	return out;
}

Json bindingSchema(const WASM::Module& module, std::string_view direction,
		std::string_view moduleName = "module")
{
	Json schema;
	schema["schema"] = "wasmjit.binding/1";
	schema["direction"] = direction;
	schema["module"] = moduleName;
	schema["types"] = Json::array();
	for (uint32_t i = 0; i < module.typeCount(); ++i) {
		Json type = typeJson(module.typeView(i));
		type["index"] = i;
		schema["types"].push_back(type);
	}
	schema["imports"] = Json::array();
	for (const WASM::ImportDescriptor& descriptor : module.importDescriptors()) {
		Json import = descriptorJson(descriptor.type);
		import["module"] = descriptor.moduleName;
		import["name"] = descriptor.fieldName;
		schema["imports"].push_back(import);
	}
	schema["exports"] = Json::array();
	for (const WASM::ExportDescriptor& descriptor : module.exportDescriptors()) {
		Json exportEntry = descriptorJson(descriptor.type);
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


std::string emitCHeader(const Json& schema)
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

const Json* findExport(const Json& schema, std::string_view name)
{
	for (const Json& entry : schema.at("exports")) {
		if (entry.value("name", "") == name)
			return &entry;
	}
	return nullptr;
}

const Json* findImport(const Json& schema, std::string_view module, std::string_view name)
{
	for (const Json& entry : schema.at("imports")) {
		if (entry.value("module", "") == module && entry.value("name", "") == name)
			return &entry;
	}
	return nullptr;
}

} // namespace

TEST_CASE("the binding schema projects core function and global descriptors", "[bindings]")
{
	const WASM::Module module = loadTestModule("asmjit_i32_basic");
	const Json schema = bindingSchema(module, "g2h");

	REQUIRE(schema.at("schema") == "wasmjit.binding/1");
	REQUIRE(schema.at("direction") == "g2h");
	REQUIRE(schema.at("imports").empty());

	const Json* add = findExport(schema, "add");
	REQUIRE(add != nullptr);
	REQUIRE(add->at("kind") == "function");
	REQUIRE(add->at("type").at("params").size() == 2u);
	REQUIRE(add->at("type").at("params")[0].at("type") == "i32");
	REQUIRE(add->at("type").at("params")[1].at("type") == "i32");
	REQUIRE(add->at("type").at("results").size() == 1u);
	REQUIRE(add->at("type").at("results")[0].at("type") == "i32");

	const Json* global = findExport(schema, "g");
	REQUIRE(global != nullptr);
	REQUIRE(global->at("kind") == "global");
	REQUIRE(global->at("type").at("type") == "i32");
	REQUIRE(global->at("mutable") == true);
}

TEST_CASE("the binding schema spells reference types opaquely", "[bindings]")
{
	const WASM::Module module = loadTestModule("wasm_js_string_length");
	const Json schema = bindingSchema(module, "h2g");

	const Json* length = findImport(schema, "wasm:js-string", "length");
	REQUIRE(length != nullptr);
	REQUIRE(length->at("kind") == "function");
	REQUIRE(length->at("type").at("params").size() == 1u);
	REQUIRE(length->at("type").at("params")[0].at("type") == "ref");
	REQUIRE(length->at("type").at("params")[0].at("heap") == "extern");
	REQUIRE(length->at("type").at("params")[0].at("nullable") == true);
	REQUIRE(length->at("type").at("results")[0].at("type") == "i32");

	const Json* identity = findExport(schema, "identity");
	REQUIRE(identity != nullptr);
	const std::string paramType =
			identity->at("type").at("params").at(0).at("type").get<std::string>();
	const std::string resultType =
			identity->at("type").at("results").at(0).at("type").get<std::string>();
	REQUIRE(paramType == "ref");
	REQUIRE(resultType == "ref");

	const std::string header = emitCHeader(schema);
	REQUIRE(header.find("typedef struct wasm_externref wasm_externref;") != std::string::npos);
	REQUIRE(header.find("wasm_externref* arg0 /* borrowed */") != std::string::npos);
	REQUIRE(header.find("/* ownership: explicit */") != std::string::npos);
	REQUIRE(header.find("typeId") == std::string::npos);
	REQUIRE(header.find("sizeof(") == std::string::npos);
}

TEST_CASE("defined references use a kind name, not a numeric TypeId, in the header", "[bindings]")
{
	const WASM::Module module = loadTestModule("plain_ref");
	const Json schema = bindingSchema(module, "g2h");

	const Json* function = findExport(schema, "f");
	REQUIRE(function != nullptr);
	const Json& param = function->at("type").at("params").at(0);
	REQUIRE(param.at("type").get<std::string>() == "ref");
	REQUIRE(param.at("heap").get<std::string>() == "defined");
	REQUIRE(param.at("typeKind").get<std::string>() == "struct");

	const std::string header = emitCHeader(schema);
	REQUIRE(header.find("typedef struct wasm_structref wasm_structref;") != std::string::npos);
	REQUIRE(header.find("wasm_structref* arg0 /* borrowed */") != std::string::npos);
	REQUIRE(header.find("wasm_ref_") == std::string::npos);
}

TEST_CASE("exact reference flags are preserved in the schema", "[bindings]")
{
	const WASM::Module module = loadTestModule("exact_ref");
	const Json schema = bindingSchema(module, "g2h");

	const Json* function = findExport(schema, "f");
	REQUIRE(function != nullptr);
	const Json& param = function->at("type").at("params").at(0);
	REQUIRE(param.at("type").get<std::string>() == "ref");
	REQUIRE(param.at("heap").get<std::string>() == "defined");
	REQUIRE(param.at("typeKind").get<std::string>() == "struct");
	REQUIRE(param.at("exact") == true);
}

TEST_CASE("the binding schema exposes GC struct and array definitions", "[bindings]")
{
	const WASM::Module module = loadTestModule("gc_refcount");
	const Json schema = bindingSchema(module, "g2g");

	bool sawStruct = false;
	bool sawArray = false;
	bool sawRefField = false;
	for (const Json& type : schema.at("types")) {
		if (type.value("kind", "") == "struct") {
			sawStruct = true;
			for (const Json& field : type.at("fields")) {
				if (field.at("type").value("type", "") == "ref")
					sawRefField = true;
			}
		}
		if (type.value("kind", "") == "array")
			sawArray = true;
	}
	REQUIRE(sawStruct);
	REQUIRE(sawArray);
	REQUIRE(sawRefField);
	REQUIRE(schema.at("direction") == "g2g");
}

