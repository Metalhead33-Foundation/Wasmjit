#include "BindingSchema.hpp"

#include "EmitInternal.hpp"
#include "EmitShared.hpp"

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

// docs/EMBEDDING_SURFACE.md §12.8: descriptor projection and the public schema
// wrappers. This is tooling, not an engine source file.
namespace WASM::Bindings {

using namespace detail;

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

//! Returns the identity a generated declaration is keyed by. Imports are
//! scoped by their Wasm module; exports share one namespace.
std::string functionEntryKey(const Json& entry, bool import)
{
	if (!entry.is_object())
		return {};
	if (import)
		return entry.value("module", std::string()) + "\x1f"
				+ entry.value("name", std::string());
	return entry.value("name", std::string());
}

//! Keeps the first declaration for each identity. A Wasm module may import the
//! same field more than once; a binding language cannot declare it twice.
void dedupeFunctionsInArray(Json& entries, bool imports)
{
	if (!entries.is_array())
		return;
	std::set<std::string> seen;
	Json unique = Json::array();
	for (const Json& entry : entries) {
		if (entry.is_object() && entry.value("kind", "") == "function") {
			const std::string key = functionEntryKey(entry, imports);
			if (!seen.insert(key).second)
				continue;
		}
		unique.push_back(entry);
	}
	entries = std::move(unique);
}

void dedupeFunctionEntries(Json& schema)
{
	if (schema.contains("imports"))
		dedupeFunctionsInArray(schema["imports"], true);
	if (schema.contains("exports"))
		dedupeFunctionsInArray(schema["exports"], false);
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
	dedupeFunctionEntries(schema);
	return schema;
}

} // namespace

Json moduleSchema(const WASM::Module& module, const WASM::TypeRegistry& types,
		Direction direction, std::string_view moduleName)
{
	return moduleSchemaImpl(module, types, direction, moduleName);
}

Schema moduleSchemaModel(const WASM::Module& module,
		const WASM::TypeRegistry& types, Direction direction,
		std::string_view moduleName)
{
	return Schema::fromJson(moduleSchemaImpl(module, types, direction, moduleName));
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
	dedupeFunctionEntries(schema);
	return schema;
}

Schema componentSchemaModel(const WASM::Component& component,
		const WASM::TypeRegistry& types, Direction direction,
		std::string_view componentName)
{
	return Schema::fromJson(
			componentSchema(component, types, direction, componentName));
}

Schema projectSchema(const Schema& input, Direction direction)
{
	if (input.id != "wasmjit.binding/1")
		throw std::runtime_error("schema import: unsupported schema id");
	if (input.direction == direction) {
		Schema out = input;
		out.dedupe();
		return out;
	}
	if (input.direction != Direction::Complete)
		throw std::runtime_error("schema import: direction '"
				+ std::string(directionName(input.direction))
				+ "' cannot be projected to '" + directionName(direction)
				+ "'; export the schema with --direction all first");

	Schema out = input;
	out.direction = direction;
	out.dedupe();
	if (direction == Direction::Complete)
		return out;
	if (direction == Direction::GuestToHost) {
		out.exports.clear();
		return out;
	}
	if (direction == Direction::HostToGuest) {
		out.imports.clear();
		return out;
	}

	// G2G: the consumer imports the producer's exports. The producer's own
	// imports are intentionally discarded.
	out.imports.clear();
	for (const Entry& exported : input.exports) {
		Entry imported = exported;
		imported.import = true;
		imported.module = input.module;
		out.imports.push_back(std::move(imported));
	}
	out.exports.clear();
	out.dedupe();
	return out;
}

Json projectSchema(const Json& input, Direction direction)
{
	return projectSchema(Schema::fromJson(input), direction).toJson();
}

std::string emitCHeader(const Schema& input)
{
	return emitCHeader(input.toJson());
}

std::string emitCHeader(const nlohmann::ordered_json& input)
{
	Json schema = input;
	dedupeFunctionEntries(schema);
	return emitCHeaderImpl(schema);
}

std::string emitRustModule(const Schema& input)
{
	return emitRustModule(input.toJson());
}



} // namespace WASM::Bindings
