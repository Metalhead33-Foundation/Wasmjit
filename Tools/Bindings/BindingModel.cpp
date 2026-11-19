#include "BindingModel.hpp"

#include "WasmBase/WasmHostType.hpp"

#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace WASM::Bindings {

namespace {

using Json = nlohmann::ordered_json;

std::string jsonString(const Json& json, std::string_view key)
{
	if (!json.is_object())
		return {};
	const auto found = json.find(std::string(key));
	return found == json.end() || !found->is_string() ? std::string()
			: found->get<std::string>();
}

bool jsonBool(const Json& json, std::string_view key, bool fallback = false)
{
	if (!json.is_object())
		return fallback;
	const auto found = json.find(std::string(key));
	return found == json.end() || !found->is_boolean() ? fallback
			: found->get<bool>();
}

Json functionTypeJson(const FunctionType& function)
{
	Json out = { { "kind", "func" } };
	out["params"] = Json::array();
	for (const ValueType& param : function.params)
		out["params"].push_back(param.toJson());
	out["results"] = Json::array();
	for (const ValueType& result : function.results)
		out["results"].push_back(result.toJson());
	return out;
}

std::string entryKey(const Entry& entry, bool import)
{
	return import ? entry.module + "\x1f" + entry.name : entry.name;
}

void dedupeEntries(std::vector<Entry>& entries, bool import)
{
	std::set<std::string> seen;
	std::vector<Entry> unique;
	unique.reserve(entries.size());
	for (Entry& entry : entries) {
		if (entry.kind == EntryKind::Function
				&& !seen.insert(entryKey(entry, import)).second)
			continue;
		unique.push_back(std::move(entry));
	}
	entries = std::move(unique);
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

bool parseDirection(std::string_view name, Direction& direction) noexcept
{
	if (name == "g2h") {
		direction = Direction::GuestToHost;
		return true;
	}
	if (name == "h2g") {
		direction = Direction::HostToGuest;
		return true;
	}
	if (name == "g2g") {
		direction = Direction::GuestToGuest;
		return true;
	}
	if (name == "all" || name == "complete" || name == "full") {
		direction = Direction::Complete;
		return true;
	}
	return false;
}

const char* entryKindName(EntryKind kind) noexcept
{
	switch (kind) {
	case EntryKind::Function: return "function";
	case EntryKind::Table: return "table";
	case EntryKind::Memory: return "memory";
	case EntryKind::Global: return "global";
	case EntryKind::Tag: return "tag";
	case EntryKind::Type: return "type";
	case EntryKind::Value: return "value";
	case EntryKind::Instance: return "instance";
	case EntryKind::Component: return "component";
	case EntryKind::CoreModule: return "core-module";
	case EntryKind::Unknown: break;
	}
	return "unknown";
}

EntryKind entryKindFromName(std::string_view name) noexcept
{
	if (name == "function") return EntryKind::Function;
	if (name == "table") return EntryKind::Table;
	if (name == "memory") return EntryKind::Memory;
	if (name == "global") return EntryKind::Global;
	if (name == "tag") return EntryKind::Tag;
	if (name == "type") return EntryKind::Type;
	if (name == "value") return EntryKind::Value;
	if (name == "instance") return EntryKind::Instance;
	if (name == "component") return EntryKind::Component;
	if (name == "core-module" || name == "core_module")
		return EntryKind::CoreModule;
	return EntryKind::Unknown;
}

const char* valueKindName(ValueKind kind) noexcept
{
	switch (kind) {
	case ValueKind::I32: return "i32";
	case ValueKind::I64: return "i64";
	case ValueKind::F32: return "f32";
	case ValueKind::F64: return "f64";
	case ValueKind::V128: return "v128";
	case ValueKind::I8: return "i8";
	case ValueKind::I16: return "i16";
	case ValueKind::U8: return "u8";
	case ValueKind::U16: return "u16";
	case ValueKind::U32: return "u32";
	case ValueKind::U64: return "u64";
	case ValueKind::S8: return "s8";
	case ValueKind::S16: return "s16";
	case ValueKind::S32: return "s32";
	case ValueKind::S64: return "s64";
	case ValueKind::Bool: return "bool";
	case ValueKind::Char: return "char";
	case ValueKind::String: return "string";
	case ValueKind::ErrorContext: return "error-context";
	case ValueKind::Ref: return "ref";
	case ValueKind::StringRef: return "stringref";
	case ValueKind::StringViewWtf8: return "stringview_wtf8";
	case ValueKind::StringViewWtf16: return "stringview_wtf16";
	case ValueKind::StringViewIter: return "stringview_iter";
	case ValueKind::Void: return "void";
	case ValueKind::Unknown: break;
	}
	return "unknown";
}

ValueKind valueKindFromName(std::string_view name) noexcept
{
	if (name == "externref" || name == "funcref" || name == "anyref"
			|| name == "eqref" || name == "i31ref" || name == "structref"
			|| name == "arrayref" || name == "exnref" || name == "contref"
			|| name == "nullref" || name == "nullfuncref"
			|| name == "nullexternref" || name == "nullexnref")
		return ValueKind::Ref;
	if (name == "i32") return ValueKind::I32;
	if (name == "i64") return ValueKind::I64;
	if (name == "f32" || name == "float32") return ValueKind::F32;
	if (name == "f64" || name == "float64") return ValueKind::F64;
	if (name == "v128") return ValueKind::V128;
	if (name == "i8") return ValueKind::I8;
	if (name == "i16") return ValueKind::I16;
	if (name == "u8") return ValueKind::U8;
	if (name == "u16") return ValueKind::U16;
	if (name == "u32") return ValueKind::U32;
	if (name == "u64") return ValueKind::U64;
	if (name == "s8") return ValueKind::S8;
	if (name == "s16") return ValueKind::S16;
	if (name == "s32") return ValueKind::S32;
	if (name == "s64") return ValueKind::S64;
	if (name == "bool") return ValueKind::Bool;
	if (name == "char") return ValueKind::Char;
	if (name == "string") return ValueKind::String;
	if (name == "error-context") return ValueKind::ErrorContext;
	if (name == "ref") return ValueKind::Ref;
	if (name == "stringref") return ValueKind::StringRef;
	if (name == "stringview_wtf8") return ValueKind::StringViewWtf8;
	if (name == "stringview_wtf16") return ValueKind::StringViewWtf16;
	if (name == "stringview_iter") return ValueKind::StringViewIter;
	if (name == "void") return ValueKind::Void;
	return ValueKind::Unknown;
}

const char* heapKindName(HeapKind kind) noexcept
{
	switch (kind) {
	case HeapKind::Func: return "func";
	case HeapKind::Extern: return "extern";
	case HeapKind::Any: return "any";
	case HeapKind::Eq: return "eq";
	case HeapKind::I31: return "i31";
	case HeapKind::Struct: return "struct";
	case HeapKind::Array: return "array";
	case HeapKind::Exn: return "exn";
	case HeapKind::Cont: return "cont";
	case HeapKind::NoCont: return "nocont";
	case HeapKind::None: return "none";
	case HeapKind::NoFunc: return "nofunc";
	case HeapKind::NoExtern: return "noextern";
	case HeapKind::NoExn: return "noexn";
	case HeapKind::Defined: return "defined";
	case HeapKind::Rec: return "rec";
	case HeapKind::Unknown: break;
	}
	return "unknown";
}

HeapKind heapKindFromName(std::string_view name) noexcept
{
	if (name == "func") return HeapKind::Func;
	if (name == "extern") return HeapKind::Extern;
	if (name == "any") return HeapKind::Any;
	if (name == "eq") return HeapKind::Eq;
	if (name == "i31") return HeapKind::I31;
	if (name == "struct") return HeapKind::Struct;
	if (name == "array") return HeapKind::Array;
	if (name == "exn") return HeapKind::Exn;
	if (name == "cont") return HeapKind::Cont;
	if (name == "nocont") return HeapKind::NoCont;
	if (name == "none") return HeapKind::None;
	if (name == "nofunc") return HeapKind::NoFunc;
	if (name == "noextern") return HeapKind::NoExtern;
	if (name == "noexn") return HeapKind::NoExn;
	if (name == "defined") return HeapKind::Defined;
	if (name == "rec") return HeapKind::Rec;
	return HeapKind::Unknown;
}

ValueType ValueType::fromJson(const Json& json)
{
	ValueType out;
	if (json.is_string()) {
		out.kind = valueKindFromName(json.get<std::string>());
		return out;
	}
	if (!json.is_object())
		return out;
	out.kind = valueKindFromName(jsonString(json, "type"));
	if (out.kind == ValueKind::Unknown)
		out.kind = valueKindFromName(jsonString(json, "kind"));
	out.packed = jsonBool(json, "packed");
	out.nullable = jsonBool(json, "nullable", true);
	out.shared = jsonBool(json, "shared");
	out.exact = jsonBool(json, "exact");
	out.wit = jsonBool(json, "wit");
	out.typeKind = jsonString(json, "typeKind");
	out.name = jsonString(json, "name");
	if (json.contains("typeId") && json.at("typeId").is_number_unsigned()) {
		out.typeId = json.at("typeId").get<std::uint32_t>();
		out.hasTypeId = true;
	}
	if (json.contains("memberIndex") && json.at("memberIndex").is_number_unsigned())
		out.memberIndex = json.at("memberIndex").get<std::uint32_t>();
	if (out.kind == ValueKind::Ref) {
		out.heap = heapKindFromName(jsonString(json, "heap"));
		if (out.heap == HeapKind::Unknown) {
			const std::string shorthand = jsonString(json, "type");
			if (shorthand == "funcref") out.heap = HeapKind::Func;
			else if (shorthand == "externref") out.heap = HeapKind::Extern;
			else if (shorthand == "anyref") out.heap = HeapKind::Any;
			else if (shorthand == "eqref") out.heap = HeapKind::Eq;
			else if (shorthand == "i31ref") out.heap = HeapKind::I31;
			else if (shorthand == "structref") out.heap = HeapKind::Struct;
			else if (shorthand == "arrayref") out.heap = HeapKind::Array;
			else if (shorthand == "exnref") out.heap = HeapKind::Exn;
			else if (shorthand == "contref") out.heap = HeapKind::Cont;
			else if (shorthand == "nullref") out.heap = HeapKind::None;
			else if (shorthand == "nullfuncref") out.heap = HeapKind::NoFunc;
			else if (shorthand == "nullexternref") out.heap = HeapKind::NoExtern;
			else if (shorthand == "nullexnref") out.heap = HeapKind::NoExn;
		}
	}
	return out;
}

Json ValueType::toJson() const
{
	if (kind == ValueKind::Ref) {
		Json out = { { "type", "ref" }, { "heap", heapKindName(heap) } };
		if (!typeKind.empty())
			out["typeKind"] = typeKind;
		if (hasTypeId)
			out["typeId"] = typeId;
		if (heap == HeapKind::Rec)
			out["memberIndex"] = memberIndex;
		if (!name.empty())
			out["name"] = name;
		out["nullable"] = nullable;
		if (shared)
			out["shared"] = true;
		if (exact)
			out["exact"] = true;
		if (wit)
			out["wit"] = true;
		return out;
	}
	Json out = { { "type", valueKindName(kind) } };
	if (packed)
		out["packed"] = true;
	return out;
}

Entry Entry::fromJson(const Json& json, bool import)
{
	Entry out;
	out.import = import;
	out.raw = json;
	if (!json.is_object())
		return out;
	out.kind = entryKindFromName(jsonString(json, "kind"));
	out.module = jsonString(json, "module");
	out.name = jsonString(json, "name");
	out.description = jsonString(json, "description");
	if (json.contains("argNames") && json.at("argNames").is_array()) {
		for (const Json& name : json.at("argNames")) {
			if (name.is_string())
				out.argNames.push_back(name.get<std::string>());
		}
	}
	if (out.kind != EntryKind::Function)
		return out;
	const bool hasAbi = json.contains("abi") && json.at("abi").is_object()
			&& json.at("abi").contains("params")
			&& json.at("abi").contains("results");
	const Json& source = hasAbi ? json.at("abi") : json.at("type");
	out.function.fromAbi = hasAbi;
	if (hasAbi) {
		out.function.indirectParams = jsonBool(json.at("abi"), "indirectParams");
		out.function.indirectResults = jsonBool(json.at("abi"), "indirectResults");
	}
	if (source.is_object()) {
		if (source.contains("params") && source.at("params").is_array()) {
			for (const Json& param : source.at("params"))
				out.function.params.push_back(ValueType::fromJson(param));
		}
		if (source.contains("results") && source.at("results").is_array()) {
			for (const Json& result : source.at("results"))
				out.function.results.push_back(ValueType::fromJson(result));
		}
	}
	return out;
}

Json Entry::toJson() const
{
	Json out = raw.is_object() ? raw : Json::object();
	out["kind"] = entryKindName(kind);
	if (!module.empty())
		out["module"] = module;
	else
		out.erase("module");
	out["name"] = name;
	if (kind == EntryKind::Function) {
		if (!description.empty())
			out["description"] = description;
		else if (!out.contains("description"))
			out["description"] = "";
		if (!argNames.empty())
			out["argNames"] = argNames;
		else if (!out.contains("argNames"))
			out["argNames"] = Json::array();
		if (!out.contains("type"))
			out["type"] = functionTypeJson(function);
	}
	return out;
}

namespace {

//! Reads an unsigned member, falling back when it is absent or mistyped.
std::uint32_t jsonUint(const Json& json, std::string_view key,
		std::uint32_t fallback = 0)
{
	if (!json.is_object())
		return fallback;
	const auto found = json.find(std::string(key));
	if (found == json.end() || !found->is_number_unsigned())
		return fallback;
	return found->get<std::uint32_t>();
}

//! The engine storage code of one reference heap kind.
WASM::ValueTypeCode referenceCode(HeapKind heap)
{
	using Code = WASM::ValueTypeCode;
	switch (heap) {
	case HeapKind::Func: return Code::FuncRef;
	case HeapKind::Extern: return Code::ExternRef;
	case HeapKind::Any: return Code::AnyRef;
	case HeapKind::Eq: return Code::EqRef;
	case HeapKind::I31: return Code::I31Ref;
	case HeapKind::Struct: return Code::StructRef;
	case HeapKind::Array: return Code::ArrayRef;
	case HeapKind::Exn: return Code::ExnRef;
	case HeapKind::Cont: return Code::ContRef;
	case HeapKind::None: return Code::NullRef;
	case HeapKind::NoFunc: return Code::NullFuncRef;
	case HeapKind::NoExtern: return Code::NullExternRef;
	case HeapKind::NoExn: return Code::NullExnRef;
	// A *defined* reference is pointer-sized like `structref`, so that is the
	// layout it has; the name stays metadata. Interning a defined reference is
	// `SO17`'s follow-up, and this table is the layout assertion, not the type.
	case HeapKind::Defined: return Code::StructRef;
	default: break;
	}
	return Code::Void;
}

//! Converts one descriptor field into the engine's own field vocabulary.
WASM::HostFieldDefinition hostField(const ObjectField& field,
		const std::string& object)
{
	WASM::HostFieldDefinition out;
	out.isMutable = field.mutableField;
	const ValueType& type = field.type;
	if (type.kind == ValueKind::Ref) {
		if (type.heap == HeapKind::Unknown || type.heap == HeapKind::Rec)
			throw std::runtime_error("object '" + object + "' field '"
					+ field.name + "': a reference needs a concrete heap kind");
		out.storage = referenceCode(type.heap);
		out.isNullable = type.nullable;
		return out;
	}
	switch (type.kind) {
	case ValueKind::I32: out.storage = WASM::ValueTypeCode::I32; break;
	case ValueKind::I64: out.storage = WASM::ValueTypeCode::I64; break;
	case ValueKind::F32: out.storage = WASM::ValueTypeCode::F32; break;
	case ValueKind::F64: out.storage = WASM::ValueTypeCode::F64; break;
	case ValueKind::V128: out.storage = WASM::ValueTypeCode::V128; break;
	case ValueKind::I8:
		out.storage = WASM::ValueTypeCode::I8;
		out.isPacked = true;
		break;
	case ValueKind::I16:
		out.storage = WASM::ValueTypeCode::I16;
		out.isPacked = true;
		break;
	default:
		throw std::runtime_error("object '" + object + "' field '" + field.name
				+ "': '" + valueKindName(type.kind)
				+ "' is not a Wasm storage type");
	}
	return out;
}

//! True when two descriptor types denote the same value.
bool sameType(const ValueType& left, const ValueType& right)
{
	return left.kind == right.kind && left.heap == right.heap
			&& left.packed == right.packed && left.nullable == right.nullable
			&& left.name == right.name;
}

//! The human spelling of a descriptor type, for refusal messages.
std::string typeLabel(const ValueType& type)
{
	std::string out = valueKindName(type.kind);
	if (type.kind == ValueKind::Ref) {
		out += " ";
		out += heapKindName(type.heap);
		if (type.heap == HeapKind::Defined && !type.name.empty())
			out += " " + type.name;
	}
	if (type.packed)
		out += " (packed)";
	return out;
}

//! The `(params) -> (results)` spelling of a method, for refusal messages.
std::string signatureLabel(const ObjectMethod& method)
{
	const auto list = [](const std::vector<ValueType>& types) {
		std::string out;
		for (std::size_t index = 0; index < types.size(); ++index) {
			if (index != 0u)
				out += ", ";
			out += typeLabel(types[index]);
		}
		return out;
	};
	return "(" + list(method.params) + ") -> (" + list(method.results) + ")";
}

//! The named method of an object, or null when it declares none by that name.
const ObjectMethod* findMethod(const ObjectDescriptor& object,
		const std::string& name)
{
	for (const ObjectMethod& method : object.methods) {
		if (method.name == name)
			return &method;
	}
	return nullptr;
}

//! Validates every descriptor against the engine's canonical layout.
//!
//! This is the load-time check the note promises: a declared offset is an
//! assertion about the canonical type, and the assertion is checked against
//! `WASM::layoutHostType` rather than trusted. `(name, version)` is unique — one
//! name and version has one shape (A.14.2) — while two versions of one name are
//! two descriptors and two types.
//!
//! A property is checked against the object *itself* rather than the layout: it
//! owns no bytes, so what can be wrong is its interface — a name that collides
//! with another member, an accessor that `methods` does not declare, or an
//! accessor whose declared signature is not the one the property's type implies.
void validateObjects(std::vector<ObjectDescriptor>& objects)
{
	std::set<std::string> keys;
	for (ObjectDescriptor& object : objects) {
		WASM::HostTypeDefinition definition;
		definition.kind = WASM::HostTypeKind::Struct;
		definition.fields.reserve(object.fields.size());
		std::set<std::string> names;
		for (const ObjectField& field : object.fields) {
			if (!names.insert(field.name).second)
				throw std::runtime_error("object '" + object.name
						+ "' declares field '" + field.name + "' twice");
			definition.fields.push_back(hostField(field, object.name));
		}
		const WASM::HostTypeLayout layout = WASM::layoutHostType(definition);
		object.size = layout.totalBytes;
		for (std::size_t index = 0; index < object.fields.size(); ++index) {
			ObjectField& field = object.fields[index];
			field.offset = layout.fields[index].offset;
			field.bytes = layout.fields[index].bytes;
			field.isReference = layout.fields[index].isReference;
			if (field.hasOffset && field.declaredOffset != field.offset)
				throw std::runtime_error("object '" + object.name + "' field '"
						+ field.name + "': declared offset "
						+ std::to_string(field.declaredOffset)
						+ " disagrees with the canonical offset "
						+ std::to_string(field.offset));
		}
		// A field, a property and a method share one member namespace: every
		// emitter projects all three as members of one resource/struct, so two
		// members with one name would collide in the generated artifact.
		std::set<std::string> methodNames;
		for (const ObjectMethod& method : object.methods) {
			if (!methodNames.insert(method.name).second)
				throw std::runtime_error("object '" + object.name
						+ "' declares method '" + method.name + "' twice");
		}
		std::set<std::string> propertyNames;
		std::set<std::string> accessors;
		for (const ObjectProperty& property : object.properties) {
			const std::string where =
					"object '" + object.name + "' property '" + property.name + "'";
			if (!propertyNames.insert(property.name).second)
				throw std::runtime_error(where + "' is declared twice");
			if (names.count(property.name) != 0u)
				throw std::runtime_error(where
						+ " has the same name as a field; a stored value and a"
						" computed one cannot share a member name");
			if (methodNames.count(property.name) != 0u)
				throw std::runtime_error(where + " has the same name as a method");
			for (const std::string* accessor : { &property.getter,
						 &property.setter }) {
				if (accessor->empty())
					continue;
				if (!accessors.insert(*accessor).second)
					throw std::runtime_error(where + ": accessor '" + *accessor
							+ "' is already claimed by another property");
				if (names.count(*accessor) != 0u)
					throw std::runtime_error(where + ": accessor '" + *accessor
							+ "' is a field, not a function");
				if (methodNames.count(*accessor) == 0u)
					throw std::runtime_error(where + ": accessor '" + *accessor
							+ "' is not declared in `methods`; an accessor is a"
							" function of the object like any other");
			}
			const ObjectMethod* getter = findMethod(object, property.getter);
			if (!getter->params.empty() || getter->results.size() != 1u
					|| !sameType(getter->results.at(0), property.type))
				throw std::runtime_error(where + ": getter '" + property.getter
						+ "' must take no parameters and return "
						+ typeLabel(property.type) + ", but `methods` declares "
						+ signatureLabel(*getter));
			if (!property.setter.empty()) {
				const ObjectMethod* setter = findMethod(object, property.setter);
				if (setter->params.size() != 1u || !setter->results.empty()
						|| !sameType(setter->params.at(0), property.type))
					throw std::runtime_error(where + ": setter '"
							+ property.setter + "' must take "
							+ typeLabel(property.type)
							+ " and return nothing, but `methods` declares "
							+ signatureLabel(*setter));
			}
		}

		const std::string key = object.name + "@" + std::to_string(object.version);
		if (!keys.insert(key).second)
			throw std::runtime_error("object descriptor '" + key
					+ "' is declared twice; one name and version has one shape");
	}
}

} // namespace

ObjectField ObjectField::fromJson(const Json& json)
{
	if (!json.is_object())
		throw std::runtime_error("object field: expected a JSON object");
	ObjectField out;
	out.name = jsonString(json, "name");
	if (out.name.empty())
		throw std::runtime_error("object field: missing name");
	Json type = json.contains("type") ? json.at("type") : Json{};
	if (type.is_null() && json.contains("kind"))
		type = json.at("kind");
	out.type = ValueType::fromJson(type);
	if (out.type.kind == ValueKind::Unknown)
		throw std::runtime_error("object field '" + out.name + "': unknown type");
	// `i8`/`i16` are only legal as packed Wasm storage, and every emitter reads
	// the flag, so it is set here rather than inferred again downstream.
	if (out.type.kind == ValueKind::I8 || out.type.kind == ValueKind::I16)
		out.type.packed = true;
	out.mutableField = jsonBool(json, "mutable");
	if (json.contains("offset")) {
		const Json& declared = json.at("offset");
		std::uint64_t value = 0;
		if (declared.is_number_unsigned())
			value = declared.get<std::uint64_t>();
		else if (declared.is_number_integer()
				&& declared.get<std::int64_t>() >= 0)
			value = static_cast<std::uint64_t>(declared.get<std::int64_t>());
		else
			throw std::runtime_error("object field '" + out.name
					+ "': offset is not a non-negative integer");
		if (value > std::numeric_limits<std::uint32_t>::max())
			throw std::runtime_error("object field '" + out.name
					+ "': offset is out of range");
		out.hasOffset = true;
		out.declaredOffset = static_cast<std::uint32_t>(value);
	}
	return out;
}

Json ObjectField::toJson() const
{
	Json out;
	out["name"] = name;
	out["type"] = type.toJson();
	out["offset"] = offset;
	out["bytes"] = bytes;
	out["reference"] = isReference;
	if (mutableField)
		out["mutable"] = true;
	return out;
}

ObjectProperty ObjectProperty::fromJson(const Json& json)
{
	if (!json.is_object())
		throw std::runtime_error("object property: expected a JSON object");
	ObjectProperty out;
	out.name = jsonString(json, "name");
	if (out.name.empty())
		throw std::runtime_error("object property: missing name");
	Json type = json.contains("type") ? json.at("type") : Json{};
	if (type.is_null() && json.contains("kind"))
		type = json.at("kind");
	out.type = ValueType::fromJson(type);
	if (out.type.kind == ValueKind::Unknown)
		throw std::runtime_error("object property '" + out.name + "': unknown type");
	if (out.type.kind == ValueKind::Void)
		throw std::runtime_error("object property '" + out.name
				+ "': a property needs a value, not `void`");
	if (out.type.kind == ValueKind::I8 || out.type.kind == ValueKind::I16)
		out.type.packed = true;
	// A property is not a slot, so an offset here is a field written the wrong
	// way round; the message names the right key rather than ignoring it.
	if (json.contains("offset"))
		throw std::runtime_error("object property '" + out.name
				+ "': a property has no storage offset; a stored slot is a `field`");
	out.getter = jsonString(json, "get");
	if (out.getter.empty())
		throw std::runtime_error("object property '" + out.name
				+ "': missing `get`; a property without a getter is a method");
	out.setter = jsonString(json, "set");
	const bool assertedMutable = jsonBool(json, "mutable");
	if (out.setter.empty() && assertedMutable)
		throw std::runtime_error("object property '" + out.name
				+ "': `mutable` without `set`; a read-only property has no setter");
	out.mutableProperty = !out.setter.empty();
	return out;
}

Json ObjectProperty::toJson() const
{
	Json out;
	out["name"] = name;
	out["type"] = type.toJson();
	out["get"] = getter;
	if (!setter.empty())
		out["set"] = setter;
	return out;
}

ObjectMethod ObjectMethod::fromJson(const Json& json)
{
	if (!json.is_object())
		throw std::runtime_error("object method: expected a JSON object");
	ObjectMethod out;
	out.name = jsonString(json, "name");
	if (out.name.empty())
		throw std::runtime_error("object method: missing name");
	const auto parseList = [&out](const Json& node, const char* key) {
		std::vector<ValueType> types;
		if (!node.contains(key))
			return types;
		const Json& array = node.at(key);
		if (!array.is_array())
			throw std::runtime_error("object method '" + out.name + "': `"
					+ key + "` must be an array");
		for (const Json& item : array) {
			ValueType type = ValueType::fromJson(item);
			if (type.kind == ValueKind::Unknown)
				throw std::runtime_error("object method '" + out.name
						+ "': unknown " + key + " type");
			types.push_back(std::move(type));
		}
		return types;
	};
	out.params = parseList(json, "params");
	out.results = parseList(json, "results");
	return out;
}

Json ObjectMethod::toJson() const
{
	Json out;
	out["name"] = name;
	out["params"] = Json::array();
	for (const ValueType& param : params)
		out["params"].push_back(param.toJson());
	out["results"] = Json::array();
	for (const ValueType& result : results)
		out["results"].push_back(result.toJson());
	return out;
}

ObjectDescriptor ObjectDescriptor::fromJson(const Json& json)
{
	if (!json.is_object())
		throw std::runtime_error("object descriptor: expected a JSON object");
	ObjectDescriptor out;
	out.name = jsonString(json, "name");
	if (out.name.empty())
		throw std::runtime_error("object descriptor: missing name");
	out.version = jsonUint(json, "version", 1);
	if (out.version == 0)
		throw std::runtime_error("object '" + out.name
				+ "': version must be at least 1");
	out.parent = jsonString(json, "parent");
	if (json.contains("interfaces") && json.at("interfaces").is_array()) {
		for (const Json& item : json.at("interfaces")) {
			if (item.is_string())
				out.interfaces.push_back(item.get<std::string>());
		}
	}
	if (json.contains("fields")) {
		if (!json.at("fields").is_array())
			throw std::runtime_error("object '" + out.name
					+ "': `fields` must be an array");
		for (const Json& item : json.at("fields"))
			out.fields.push_back(ObjectField::fromJson(item));
	}
	if (json.contains("properties")) {
		if (!json.at("properties").is_array())
			throw std::runtime_error("object '" + out.name
					+ "': `properties` must be an array");
		for (const Json& item : json.at("properties"))
			out.properties.push_back(ObjectProperty::fromJson(item));
	}
	if (json.contains("methods")) {
		if (!json.at("methods").is_array())
			throw std::runtime_error("object '" + out.name
					+ "': `methods` must be an array");
		for (const Json& item : json.at("methods"))
			out.methods.push_back(ObjectMethod::fromJson(item));
	}
	out.raw = json;
	return out;
}

Json ObjectDescriptor::toJson() const
{
	Json out = raw.is_object() ? raw : Json::object();
	out["name"] = name;
	out["version"] = version;
	if (parent.empty())
		out.erase("parent");
	else
		out["parent"] = parent;
	if (interfaces.empty())
		out.erase("interfaces");
	else
		out["interfaces"] = interfaces;
	out["size"] = size;
	out["fields"] = Json::array();
	for (const ObjectField& field : fields)
		out["fields"].push_back(field.toJson());
	if (!properties.empty()) {
		out["properties"] = Json::array();
		for (const ObjectProperty& property : properties)
			out["properties"].push_back(property.toJson());
	} else {
		out.erase("properties");
	}
	out["methods"] = Json::array();
	for (const ObjectMethod& method : methods)
		out["methods"].push_back(method.toJson());
	return out;
}
Schema Schema::fromJson(const Json& json)
{
	if (!json.is_object())
		throw std::runtime_error("schema import: expected a JSON object");
	Schema out;
	out.id = jsonString(json, "schema");
	if (out.id.empty())
		out.id = "wasmjit.binding/1";
	// The envelope is versioned: a consumer that emits nothing but functions
	// keeps speaking /1, and an `objects` section opts into /2 (A.14.5). An
	// unknown version is a load-time error rather than a best-effort parse.
	if (out.id != "wasmjit.binding/1" && out.id != "wasmjit.binding/2")
		throw std::runtime_error("schema import: unsupported schema version '"
				+ out.id + "'; this generator speaks wasmjit.binding/1 and"
				" wasmjit.binding/2");
	const std::string direction = jsonString(json, "direction");
	if (!parseDirection(direction, out.direction))
		throw std::runtime_error("schema import: missing or unknown direction");
	out.module = jsonString(json, "module");
	if (out.module.empty())
		out.module = "module";
	out.frontend = jsonString(json, "frontend");
	out.extras = json;
	out.types = json.contains("types") ? json.at("types") : Json::array();
	if (json.contains("imports") && json.at("imports").is_array()) {
		for (const Json& entry : json.at("imports"))
			out.imports.push_back(Entry::fromJson(entry, true));
	}
	if (json.contains("exports") && json.at("exports").is_array()) {
		for (const Json& entry : json.at("exports"))
			out.exports.push_back(Entry::fromJson(entry, false));
	}
	if (json.contains("objects")) {
		if (!json.at("objects").is_array())
			throw std::runtime_error("schema import: `objects` must be an array");
		if (!json.at("objects").empty()) {
			if (out.id != "wasmjit.binding/2")
				throw std::runtime_error("schema import: an `objects` section"
						" requires wasmjit.binding/2, not '" + out.id + "'");
			for (const Json& item : json.at("objects"))
				out.objects.push_back(ObjectDescriptor::fromJson(item));
			validateObjects(out.objects);
		}
	}
	out.dedupe();
	return out;
}

Json Schema::toJson() const
{
	Json out = extras.is_object() ? extras : Json::object();
	// An object descriptor is what makes this envelope `/2`; a `/2` input that
	// carries no objects stays `/2` so a round-trip never downgrades silently.
	out["schema"] = objects.empty() ? id : "wasmjit.binding/2";
	out["direction"] = directionName(direction);
	out["module"] = module;
	if (!frontend.empty())
		out["frontend"] = frontend;
	if (!types.is_null())
		out["types"] = types;
	if (objects.empty()) {
		out.erase("objects");
	} else {
		out["objects"] = Json::array();
		for (const ObjectDescriptor& object : objects)
			out["objects"].push_back(object.toJson());
	}
	out["imports"] = Json::array();
	for (const Entry& entry : imports)
		out["imports"].push_back(entry.toJson());
	out["exports"] = Json::array();
	for (const Entry& entry : exports)
		out["exports"].push_back(entry.toJson());
	return out;
}

void Schema::dedupe()
{
	dedupeEntries(imports, true);
	dedupeEntries(exports, false);
}

} // namespace WASM::Bindings
