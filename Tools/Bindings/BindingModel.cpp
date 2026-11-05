#include "BindingModel.hpp"

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

Schema Schema::fromJson(const Json& json)
{
	if (!json.is_object())
		throw std::runtime_error("schema import: expected a JSON object");
	Schema out;
	out.id = jsonString(json, "schema");
	if (out.id.empty())
		out.id = "wasmjit.binding/1";
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
	out.dedupe();
	return out;
}

Json Schema::toJson() const
{
	Json out = extras.is_object() ? extras : Json::object();
	out["schema"] = id;
	out["direction"] = directionName(direction);
	out["module"] = module;
	if (!frontend.empty())
		out["frontend"] = frontend;
	if (!types.is_null())
		out["types"] = types;
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
