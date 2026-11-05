#include "WitFrontend.hpp"

#include "BindingSchema.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace WASM::Bindings {

namespace {

using Json = nlohmann::ordered_json;

std::string readText(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	std::ostringstream text;
	text << input.rdbuf();
	return text.str();
}

std::string stringAt(const Json& object, std::string_view key)
{
	if (!object.is_object())
		return {};
	const auto found = object.find(std::string(key));
	return found == object.end() || !found->is_string() ? std::string()
			: found->get<std::string>();
}

Json opaqueWitType(const std::string& name)
{
	Json out = { { "type", "ref" }, { "heap", "extern" },
		{ "nullable", true }, { "wit", true } };
	if (!name.empty())
		out["name"] = name;
	return out;
}

Json scalarWitType(const std::string& type)
{
	for (const char* scalar : { "bool", "u8", "s8", "u16", "s16", "u32",
			"s32", "u64", "s64", "f32", "f64", "char", "string",
			"error-context" })
		if (type == scalar)
			return Json{ { "type", type } };
	if (type == "unit")
		return Json{ { "type", "void" } };
	return {};
}

std::string witNamePart(std::string_view text)
{
	std::string out;
	for (const char c : text)
		out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
	if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())))
		out.insert(0, "x_");
	return out;
}

std::string qualifiedInterfaceName(std::string packageName,
		const std::string& interfaceName);

std::string witTypeReferenceName(const Json& type, const Json& document,
		unsigned depth);

std::string witTypeOwnerPrefix(const Json& definition, const Json& document)
{
	if (!definition.is_object() || !definition.contains("owner")
			|| !definition.at("owner").is_object())
		return {};
	const Json& owner = definition.at("owner");
	if (!owner.contains("interface") || !owner.at("interface").is_number_integer())
		return {};
	const int interfaceId = owner.at("interface").get<int>();
	const auto interfaces = document.find("interfaces");
	if (interfaces == document.end() || !interfaces->is_array()
			|| interfaceId < 0
			|| static_cast<std::size_t>(interfaceId) >= interfaces->size())
		return {};
	const Json& interfaceEntry = interfaces->at(static_cast<std::size_t>(interfaceId));
	const std::string interfaceName = stringAt(interfaceEntry, "name");
	const int packageId = interfaceEntry.value("package", -1);
	std::string packageName;
	if (packageId >= 0 && document.contains("packages")
			&& document.at("packages").is_array()
			&& static_cast<std::size_t>(packageId) < document.at("packages").size())
		packageName = stringAt(document.at("packages").at(
				static_cast<std::size_t>(packageId)), "name");
	if (packageName.empty() || interfaceName.empty())
		return {};
	return witNamePart(qualifiedInterfaceName(packageName, interfaceName));
}

std::string witKindName(const Json& kind, const Json& document, unsigned depth)
{
	if (depth > 32)
		return "recursive";
	if (kind.is_string())
		return witNamePart(kind.get<std::string>());
	if (!kind.is_object())
		return "unknown";
	if (kind.contains("type"))
		return witTypeReferenceName(kind.at("type"), document, depth + 1);
	if (kind.contains("record")) {
		std::string out = "record";
		const auto fields = kind.at("record").find("fields");
		if (fields != kind.at("record").end() && fields->is_array())
			for (const Json& field : *fields)
				out += "_" + witNamePart(field.value("name", std::string()));
		return out;
	}
	if (kind.contains("tuple")) {
		std::string out = "tuple";
		const auto types = kind.at("tuple").find("types");
		if (types != kind.at("tuple").end() && types->is_array())
			for (const Json& item : *types)
				out += "_" + witTypeReferenceName(item, document, depth + 1);
		return out;
	}
	if (kind.contains("list"))
		return "list_" + witTypeReferenceName(kind.at("list"), document,
				depth + 1);
	if (kind.contains("option"))
		return "option_" + witTypeReferenceName(kind.at("option"), document,
				depth + 1);
	if (kind.contains("result")) {
		std::string out = "result";
		const Json& result = kind.at("result");
		out += "_" + (result.contains("ok") && !result.at("ok").is_null()
				? witTypeReferenceName(result.at("ok"), document, depth + 1)
				: std::string("none"));
		out += "_" + (result.contains("err") && !result.at("err").is_null()
				? witTypeReferenceName(result.at("err"), document, depth + 1)
				: std::string("none"));
		return out;
	}
	if (kind.contains("variant")) {
		std::string out = "variant";
		const auto cases = kind.at("variant").find("cases");
		if (cases != kind.at("variant").end() && cases->is_array())
			for (const Json& alternative : *cases)
				out += "_" + witNamePart(alternative.value("name", std::string()));
		return out;
	}
	if (kind.contains("handle")) {
		const Json& handle = kind.at("handle");
		if (handle.contains("own"))
			return "own_" + witTypeReferenceName(handle.at("own"), document,
					depth + 1);
		if (handle.contains("borrow"))
			return "borrow_" + witTypeReferenceName(handle.at("borrow"), document,
					depth + 1);
		return "handle";
	}
	if (kind.contains("stream"))
		return "stream_" + witTypeReferenceName(kind.at("stream"), document,
				depth + 1);
	if (kind.contains("future"))
		return "future_" + witTypeReferenceName(kind.at("future"), document,
				depth + 1);
	if (kind.contains("enum"))
		return "enum";
	if (kind.contains("flags"))
		return "flags";
	return "anonymous";
}

std::string witTypeReferenceName(const Json& type, const Json& document,
		unsigned depth)
{
	if (depth > 32)
		return "recursive";
	if (type.is_null())
		return "none";
	if (type.is_string())
		return witNamePart(type.get<std::string>());
	if (type.is_number_integer()) {
		const auto id = type.get<std::size_t>();
		const auto types = document.find("types");
		if (types == document.end() || !types->is_array()
				|| id >= types->size())
			return "type_" + std::to_string(id);
		const Json& definition = types->at(id);
		if (definition.contains("witName") && definition.at("witName").is_string())
			return definition.at("witName").get<std::string>();
		if (definition.contains("kind") && definition.at("kind").is_object()
				&& definition.at("kind").contains("type"))
			return witTypeReferenceName(definition.at("kind").at("type"),
					document, depth + 1);
		const std::string name = stringAt(definition, "name");
		if (!name.empty()) {
			const std::string prefix = witTypeOwnerPrefix(definition, document);
			return prefix.empty() ? witNamePart(name)
					: prefix + "_" + witNamePart(name);
		}
		if (definition.contains("kind"))
			return witKindName(definition.at("kind"), document, depth + 1)
					+ "_" + std::to_string(id);
		return "type_" + std::to_string(id);
	}
	if (type.is_object() && type.contains("type"))
		return witTypeReferenceName(type.at("type"), document, depth + 1);
	return "anonymous";
}

Json resolveWitType(const Json& type, const Json& document, unsigned depth);

Json resolveWitKind(const Json& kind, const Json& document, unsigned depth)
{
	if (!kind.is_object())
		return resolveWitType(kind, document, depth + 1);
	if (kind.contains("type"))
		return resolveWitType(kind.at("type"), document, depth + 1);
	return opaqueWitType(kind.value("name", std::string()));
}

Json resolveWitType(const Json& type, const Json& document, unsigned depth)
{
	if (depth > 32)
		return opaqueWitType("recursive");
	if (type.is_string()) {
		const Json scalar = scalarWitType(type.get<std::string>());
		return scalar.is_null() ? opaqueWitType(type.get<std::string>())
								: scalar;
	}
	if (type.is_number_integer()) {
		const auto id = type.get<std::size_t>();
		const auto types = document.find("types");
		if (types == document.end() || !types->is_array()
				|| id >= types->size())
			return opaqueWitType("type-" + std::to_string(id));
		const Json& definition = types->at(id);
		if (definition.contains("kind")) {
			const Json resolved = resolveWitKind(definition.at("kind"),
					document, depth + 1);
			// An alias to a scalar keeps the scalar type; a structured type
			// keeps its semantic name instead of leaking an internal id into a
			// target-language identifier.
			if (resolved.value("type", "") != "ref")
				return resolved;
			return opaqueWitType(witTypeReferenceName(type, document, depth + 1));
		}
		return opaqueWitType(witTypeReferenceName(type, document, depth + 1));
	}
	if (type.is_object() && type.contains("type"))
		return resolveWitType(type.at("type"), document, depth + 1);
	return opaqueWitType(type.is_object()
			? type.value("name", std::string()) : type.dump());
}

std::string documentation(const Json& function)
{
	if (!function.contains("docs"))
		return {};
	const Json& docs = function.at("docs");
	if (docs.is_string())
		return docs.get<std::string>();
	if (docs.is_object()) {
		if (docs.contains("contents") && docs.at("contents").is_string())
			return docs.at("contents").get<std::string>();
		if (docs.contains("text") && docs.at("text").is_string())
			return docs.at("text").get<std::string>();
	}
	return {};
}

std::vector<std::string> flattenWitType(const Json& type, const Json& document,
		unsigned depth);

std::vector<std::string> flattenWitKind(const Json& kind, const Json& document,
		unsigned depth)
{
	if (depth > 32)
		return { "i32" };
	if (kind.contains("type"))
		return flattenWitType(kind.at("type"), document, depth + 1);
	if (kind.contains("record")) {
		std::vector<std::string> out;
		const auto fields = kind.at("record").find("fields");
		if (fields != kind.at("record").end() && fields->is_array()) {
			for (const Json& field : *fields) {
				const std::vector<std::string> flat = flattenWitType(
						field.value("type", Json{}), document, depth + 1);
				out.insert(out.end(), flat.begin(), flat.end());
			}
		}
		return out;
	}
	if (kind.contains("tuple")) {
		std::vector<std::string> out;
		const auto types = kind.at("tuple").find("types");
		if (types != kind.at("tuple").end() && types->is_array()) {
			for (const Json& item : *types) {
				const std::vector<std::string> flat =
						flattenWitType(item, document, depth + 1);
				out.insert(out.end(), flat.begin(), flat.end());
			}
		}
		return out;
	}
	if (kind.contains("list") || kind.contains("string"))
		return { "i32", "i32" };
	if (kind.contains("flags")) {
		std::size_t count = 0;
		const auto values = kind.at("flags").find("flags");
		if (values != kind.at("flags").end() && values->is_array())
			count = values->size();
		return std::vector<std::string>((count + 31u) / 32u, "i32");
	}
	if (kind.contains("enum"))
		return { "i32" };
	if (kind.contains("option")) {
		std::vector<std::string> out = { "i32" };
		const std::vector<std::string> payload =
				flattenWitType(kind.at("option"), document, depth + 1);
		out.insert(out.end(), payload.begin(), payload.end());
		return out;
	}
	if (kind.contains("result")) {
		std::vector<std::string> out = { "i32" };
		std::vector<std::string> ok;
		std::vector<std::string> error;
		if (kind.at("result").contains("ok"))
			ok = flattenWitType(kind.at("result").at("ok"), document, depth + 1);
		if (kind.at("result").contains("err"))
			error = flattenWitType(kind.at("result").at("err"), document,
					depth + 1);
		const std::size_t width = std::max(ok.size(), error.size());
		for (std::size_t i = 0; i < width; ++i) {
			const std::string left = i < ok.size() ? ok[i] : "i32";
			const std::string right = i < error.size() ? error[i] : "i32";
			out.push_back(left == right ? left
					: (left == "f64" || right == "f64" ? "f64"
					: (left == "i64" || right == "i64" ? "i64" : "i32")));
		}
		return out;
	}
	if (kind.contains("variant")) {
		std::vector<std::string> payload;
		const auto cases = kind.at("variant").find("cases");
		if (cases != kind.at("variant").end() && cases->is_array()) {
			for (const Json& alternative : *cases) {
				if (!alternative.contains("type")
						|| alternative.at("type").is_null())
					continue;
				const std::vector<std::string> flat = flattenWitType(
						alternative.at("type"), document, depth + 1);
				const std::size_t width = std::max(payload.size(), flat.size());
				for (std::size_t i = 0; i < width; ++i) {
					const std::string left = i < payload.size() ? payload[i] : "i32";
					const std::string right = i < flat.size() ? flat[i] : "i32";
					if (i >= payload.size())
						payload.push_back(right);
					else if (payload[i] != right)
						payload[i] = left == "f64" || right == "f64" ? "f64"
								: (left == "i64" || right == "i64" ? "i64"
								: "i32");
				}
			}
		}
		std::vector<std::string> out = { "i32" };
		out.insert(out.end(), payload.begin(), payload.end());
		return out;
	}
	if (kind.contains("handle") || kind.contains("resource")
			|| kind.contains("stream") || kind.contains("future"))
		return { "i32" };
	return { "i32" };
}

std::vector<std::string> flattenWitType(const Json& type, const Json& document,
		unsigned depth)
{
	if (depth > 32)
		return { "i32" };
	if (type.is_null())
		return {};
	if (type.is_string()) {
		const std::string name = type.get<std::string>();
		if (name == "unit" || name == "void")
			return {};
		if (name == "string")
			return { "i32", "i32" };
		if (name == "error-context" || name == "bool" || name == "u8"
				|| name == "s8" || name == "u16" || name == "s16"
				|| name == "u32" || name == "s32" || name == "char")
			return { "i32" };
		if (name == "u64" || name == "s64")
			return { "i64" };
		if (name == "f32")
			return { "f32" };
		if (name == "f64")
			return { "f64" };
		return { "i32" };
	}
	if (type.is_number_integer()) {
		const auto id = type.get<std::size_t>();
		const auto types = document.find("types");
		if (types == document.end() || !types->is_array()
				|| id >= types->size())
			return { "i32" };
		const Json& definition = types->at(id);
		if (definition.contains("kind"))
			return flattenWitKind(definition.at("kind"), document, depth + 1);
		return { "i32" };
	}
	if (type.is_object() && type.contains("type"))
		return flattenWitType(type.at("type"), document, depth + 1);
	return { "i32" };
}

Json coreWitAbiType(const std::string& type)
{
	return Json{ { "type", type } };
}

Json canonicalWitAbi(const Json& function, const Json& document)
{
	Json abi;
	abi["params"] = Json::array();
	abi["results"] = Json::array();
	std::vector<std::string> params;
	if (function.contains("params") && function.at("params").is_array()) {
		for (const Json& parameter : function.at("params")) {
			if (!parameter.is_object())
				continue;
			const std::vector<std::string> flat = flattenWitType(
					parameter.value("type", Json{}), document, 0);
			params.insert(params.end(), flat.begin(), flat.end());
		}
	}
	std::vector<std::string> results;
	if (function.contains("result") && !function.at("result").is_null())
		results = flattenWitType(function.at("result"), document, 0);

	abi["indirectParams"] = params.size() > 16u;
	if (params.size() > 16u)
		params = { "i32" };
	abi["indirectResults"] = results.size() > 1u;
	if (results.size() > 1u)
		results = { "i32" };
	for (const std::string& type : params)
		abi["params"].push_back(coreWitAbiType(type));
	for (const std::string& type : results)
		abi["results"].push_back(coreWitAbiType(type));
	return abi;
}

bool witFunctionIsAsync(const Json& function)
{
	if (!function.is_object() || !function.contains("kind"))
		return false;
	const Json& kind = function.at("kind");
	if (kind.is_string())
		return kind.get<std::string>().rfind("async", 0) == 0;
	if (kind.is_object()) {
		for (const auto& [name, ignored] : kind.items()) {
			(void)ignored;
			if (name.rfind("async", 0) == 0)
				return true;
		}
	}
	return false;
}

void appendFunction(const Json& function, const Json& document,
		const std::string& fallbackName, bool import, const std::string& module,
		Schema& schema)
{
	Entry entry;
	entry.kind = EntryKind::Function;
	entry.import = import;
	entry.module = module;
	entry.name = stringAt(function, "name");
	if (entry.name.empty())
		entry.name = fallbackName;
	if (entry.name.empty())
		return;
	entry.description = documentation(function);

	Json params = Json::array();
	Json results = Json::array();
	if (function.contains("params") && function.at("params").is_array()) {
		std::size_t index = 0;
		for (const Json& parameter : function.at("params")) {
			if (!parameter.is_object())
				continue;
			const Json type = resolveWitType(
					parameter.value("type", Json{}), document, 0);
			params.push_back(type);
			entry.function.params.push_back(ValueType::fromJson(type));
			std::string name = stringAt(parameter, "name");
			if (name.empty())
				name = "arg" + std::to_string(index);
			entry.argNames.push_back(std::move(name));
			++index;
		}
	}
	if (function.contains("result")) {
		const Json& result = function.at("result");
		if (!result.is_null()) {
			const Json type = resolveWitType(result, document, 0);
			results.push_back(type);
			entry.function.results.push_back(ValueType::fromJson(type));
		}
	}

	Json type = { { "kind", "func" }, { "params", std::move(params) },
		{ "results", std::move(results) } };
	entry.raw = {
		{ "kind", "function" },
		{ "name", entry.name },
		{ "description", entry.description },
		{ "argNames", entry.argNames },
		{ "type", std::move(type) },
	};
	entry.raw["abi"] = canonicalWitAbi(function, document);
	if (function.contains("stability"))
		entry.raw["stability"] = function.at("stability");
	if (witFunctionIsAsync(function)) {
		entry.raw["witAsync"] = true;
		schema.extras["witAsync"] = true;
	}
	if (import)
		entry.raw["module"] = entry.module;
	if (import)
		schema.imports.push_back(std::move(entry));
	else
		schema.exports.push_back(std::move(entry));
}

std::string qualifiedInterfaceName(std::string packageName,
		const std::string& interfaceName)
{
	if (packageName.empty())
		return interfaceName;
	const auto version = packageName.find('@');
	if (version == std::string::npos)
		return packageName + "/" + interfaceName;
	packageName.insert(version, "/" + interfaceName);
	return packageName;
}

void appendInterface(const Json& interfaceEntry, const Json& document,
		bool import, const std::string& interfaceModule, Schema& schema)
{
	const std::string interfaceName = stringAt(interfaceEntry, "name");
	const std::string module = import
			? (interfaceModule.empty() ? interfaceName : interfaceModule)
			: std::string();
	const auto functions = interfaceEntry.find("functions");
	if (functions == interfaceEntry.end() || !functions->is_object())
		return;
	for (const auto& [name, function] : functions->items())
		appendFunction(function, document, name, import, module, schema);
}

const Json* resolveInterface(const Json& reference,
		std::map<int, const Json*>& interfaces, std::string& error)
{
	if (!reference.is_object() || !reference.contains("interface"))
		return nullptr;
	const Json& identity = reference.at("interface");
	if (!identity.is_object())
		return nullptr;
	const int id = identity.value("id", -1);
	const auto found = interfaces.find(id);
	if (found == interfaces.end()) {
		error = "WIT world references an interface with no matching id";
		return nullptr;
	}
	return found->second;
}

void appendWorldEntry(const Json& node, const Json& document, const std::string& key,
		bool import, const std::string& moduleName,
		std::map<int, const Json*>& interfaces,
		const std::map<int, std::string>& interfaceModules, Schema& schema,
		std::string& error)
{
	if (node.contains("function")) {
		appendFunction(node.at("function"), document, key, import,
				import ? moduleName : std::string(), schema);
		return;
	}
	if (const Json* interfaceEntry = resolveInterface(node, interfaces, error)) {
		int id = -1;
		if (node.contains("interface") && node.at("interface").is_object())
			id = node.at("interface").value("id", -1);
		const auto module = interfaceModules.find(id);
		appendInterface(*interfaceEntry, document, import,
				module == interfaceModules.end() ? std::string() : module->second,
				schema);
		return;
	}
	if (!error.empty())
		return;

	Entry entry;
	entry.import = import;
	entry.module = import ? moduleName : std::string();
	entry.name = key;
	entry.kind = node.contains("type") ? EntryKind::Type
			: node.contains("instance") ? EntryKind::Instance : EntryKind::Value;
	entry.raw = { { "kind", entryKindName(entry.kind) }, { "name", key } };
	if (import)
		entry.raw["module"] = moduleName;
	if (import)
		schema.imports.push_back(std::move(entry));
	else
		schema.exports.push_back(std::move(entry));
}

} // namespace

bool parseWitModel(const Json& document, WitModel& model, std::string& error)
{
	error.clear();
	model = {};
	if (!document.is_object()) {
		error = "WIT JSON: expected an object";
		return false;
	}

	if (document.contains("types") && document.at("types").is_array()) {
		std::size_t id = 0;
		for (const Json& definition : document.at("types")) {
			WitTypeInfo type;
			type.id = id++;
			if (!definition.is_object()) {
				error = "WIT JSON: type-table entry is not an object";
				return false;
			}
			type.name = stringAt(definition, "name");
			type.kind = definition.contains("kind") ? definition.at("kind")
					: Json();
			type.alias = definition.contains("kind")
					&& definition.at("kind").is_object()
					&& definition.at("kind").contains("type");
			if (definition.contains("owner")
					&& definition.at("owner").is_object()
					&& definition.at("owner").contains("interface")
					&& definition.at("owner").at("interface").is_number_integer())
				type.ownerInterface = definition.at("owner").at("interface")
						.get<int>();
			if (definition.contains("witName"))
				type.displayName = stringAt(definition, "witName");
			else
				type.displayName = witTypeReferenceName(Json(id - 1), document, 0);
			model.types.push_back(std::move(type));
		}
	}

	if (document.contains("interfaces") && document.at("interfaces").is_array()) {
		std::size_t id = 0;
		for (const Json& definition : document.at("interfaces")) {
			WitInterfaceInfo interface;
			interface.id = id++;
			interface.name = stringAt(definition, "name");
			interface.package = definition.value("package", -1);
			const auto functions = definition.find("functions");
			if (functions != definition.end() && functions->is_object()) {
				for (const auto& [name, function] : functions->items()) {
					WitFunctionInfo info;
					info.name = name;
					info.raw = function;
					info.async = witFunctionIsAsync(function);
					interface.functions.push_back(std::move(info));
				}
			}
			const auto types = definition.find("types");
			if (types != definition.end() && types->is_object()) {
				for (const auto& [name, type] : types->items()) {
					(void)name;
					if (type.is_number_integer())
						interface.types.push_back(type.get<std::size_t>());
				}
			}
			model.interfaces.push_back(std::move(interface));
		}
	}

	for (WitTypeInfo& type : model.types) {
		if (type.ownerInterface >= 0
				&& static_cast<std::size_t>(type.ownerInterface) < model.interfaces.size())
			type.ownerPackage =
					model.interfaces[static_cast<std::size_t>(type.ownerInterface)].package;
	}

	if (document.contains("packages") && document.at("packages").is_array()) {
		std::size_t id = 0;
		for (const Json& definition : document.at("packages")) {
			WitPackageInfo package;
			package.id = id++;
			package.name = stringAt(definition, "name");
			const auto interfaces = definition.find("interfaces");
			if (interfaces != definition.end() && interfaces->is_object())
				for (const auto& [name, value] : interfaces->items()) {
					(void)name;
					if (value.is_number_integer())
						package.interfaces.push_back(value.get<std::size_t>());
				}
			const auto worlds = definition.find("worlds");
			if (worlds != definition.end() && worlds->is_object())
				for (const auto& [name, value] : worlds->items()) {
					(void)name;
					if (value.is_number_integer())
						package.worlds.push_back(value.get<std::size_t>());
				}
			model.packages.push_back(std::move(package));
		}
	}

	if (document.contains("worlds") && document.at("worlds").is_array()) {
		std::size_t id = 0;
		for (const Json& definition : document.at("worlds")) {
			WitWorldInfo world;
			world.id = id++;
			world.name = stringAt(definition, "name");
			world.package = definition.value("package", -1);
			world.imports = definition.contains("imports")
					? definition.at("imports") : Json::object();
			world.exports = definition.contains("exports")
					? definition.at("exports") : Json::object();
			model.worlds.push_back(std::move(world));
		}
	}

	return true;
}

Json witResolveType(const Json& document, const Json& type)
{
	return resolveWitType(type, document, 0);
}

std::string witTypeDisplayName(const Json& document, std::size_t id)
{
	return witTypeReferenceName(Json(static_cast<std::uint64_t>(id)), document,
			0);
}

Schema witSchema(const Json& document, Direction direction,
		std::string_view moduleName, std::string_view worldName,
		std::string& error, WitAsyncPolicy asyncPolicy,
		std::string_view packageName)
{
	error.clear();
	if (!document.is_object()) {
		error = "WIT JSON: expected an object";
		return {};
	}
	const auto worlds = document.find("worlds");
	if (worlds == document.end() || !worlds->is_array() || worlds->empty()) {
		error = "WIT JSON: no worlds";
		return {};
	}

	WitModel model;
	if (!parseWitModel(document, model, error))
		return {};

	const auto packageNameFor = [&](const WitWorldInfo& world) -> std::string {
		if (world.package < 0
				|| static_cast<std::size_t>(world.package) >= model.packages.size())
			return {};
		return model.packages[static_cast<std::size_t>(world.package)].name;
	};
	const WitWorldInfo* selected = nullptr;
	for (const WitWorldInfo& world : model.worlds) {
		const bool worldMatches = worldName.empty()
				? packageName.empty() || packageNameFor(world) == packageName
				: world.name == worldName
				&& (packageName.empty() || packageNameFor(world) == packageName);
		if (worldMatches) {
			selected = &world;
			break;
		}
	}
	if (selected == nullptr) {
		error = packageName.empty()
				? "WIT JSON: requested world was not found"
				: "WIT JSON: requested world was not found in package '"
						+ std::string(packageName) + "'";
		return {};
	}

	std::string schemaName(moduleName);
	if (schemaName.empty()) {
		if (selected->package >= 0
				&& static_cast<std::size_t>(selected->package) < model.packages.size())
			schemaName = model.packages[static_cast<std::size_t>(selected->package)].name;
		if (schemaName.empty() && !model.packages.empty())
			schemaName = model.packages.front().name;
		if (schemaName.empty())
			schemaName = selected->name;
		if (schemaName.empty())
			schemaName = "component";
	}

	std::map<int, const Json*> interfaces;
	if (document.contains("interfaces") && document.at("interfaces").is_array()) {
		for (std::size_t i = 0; i < document.at("interfaces").size(); ++i)
			interfaces.emplace(static_cast<int>(i), &document.at("interfaces")[i]);
	}

	std::map<int, std::string> interfaceModules;
	for (const WitPackageInfo& package : model.packages)
		for (const std::size_t id : package.interfaces)
			if (id < model.interfaces.size())
				interfaceModules.emplace(static_cast<int>(id),
						qualifiedInterfaceName(package.name,
								model.interfaces[id].name));

	Schema schema;
	schema.module = schemaName;
	schema.direction = Direction::Complete;
	schema.extras = { { "kind", "component" }, { "frontend", "wit" },
		{ "world", selected->name }, { "witPackages", model.packages.size() },
		{ "witInterfaces", model.interfaces.size() },
		{ "witTypes", model.types.size() } };
	if (document.contains("types")) {
		schema.types = document.at("types");
		for (std::size_t i = 0; i < schema.types.size(); ++i)
			schema.types[i]["witName"] = i < model.types.size()
					? model.types[i].displayName : witTypeDisplayName(document, i);
	}

	for (const auto& [name, node] : selected->imports.items())
		appendWorldEntry(node, document, name, true, schemaName, interfaces,
				interfaceModules, schema, error);
	for (const auto& [name, node] : selected->exports.items())
		appendWorldEntry(node, document, name, false, schemaName, interfaces,
				interfaceModules, schema, error);
	if (!error.empty())
		return {};
	if (asyncPolicy == WitAsyncPolicy::Reject) {
		const auto rejectAsync = [&](const std::vector<Entry>& entries) {
			for (const Entry& entry : entries) {
				if (entry.raw.value("witAsync", false)) {
					error = "WIT async function '" + entry.name
							+ "' is rejected by the selected async policy; "
							  "remove --wit-reject-async or select "
							  "WitAsyncPolicy::AllowSynchronous";
					return true;
				}
			}
			return false;
		};
		if (rejectAsync(schema.imports) || rejectAsync(schema.exports))
			return {};
	}
	schema.dedupe();
	try {
		return projectSchema(schema, direction);
	} catch (const std::exception& exception) {
		error = exception.what();
		return {};
	}
}

Schema witSchemaFromFile(const std::filesystem::path& path, Direction direction,
		std::string_view moduleName, std::string_view worldName, bool forceJson,
		std::string& error, WitAsyncPolicy asyncPolicy, std::string_view packageName)
{
	error.clear();
	if (!std::filesystem::is_regular_file(path)
			&& !std::filesystem::is_directory(path)) {
		error = "not a WIT file or package directory: " + path.string();
		return {};
	}

	const bool jsonFile = forceJson || path.extension() == ".json";
	if (!jsonFile) {
		const char* configured = std::getenv("WASM_TOOLS");
		const std::string tool = configured == nullptr ? "wasm-tools" : configured;
		const auto stamp = std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count());
		const std::filesystem::path jsonPath = std::filesystem::temp_directory_path()
				/ ("wasmjit-wit-" + stamp + ".json");
		const std::filesystem::path logPath = std::filesystem::temp_directory_path()
				/ ("wasmjit-wit-" + stamp + ".log");
		const std::string command = "\"" + tool + "\" component wit \""
				+ std::filesystem::absolute(path).string() + "\" --json > \""
				+ jsonPath.string() + "\" 2> \"" + logPath.string() + "\"";
		const int status = std::system(command.c_str());
		if (status != 0) {
			error = "wasm-tools component wit failed: " + readText(logPath);
			std::filesystem::remove(jsonPath);
			std::filesystem::remove(logPath);
			return {};
		}
		Schema schema;
		try {
			schema = witSchema(Json::parse(readText(jsonPath)), direction,
					moduleName, worldName, error, asyncPolicy, packageName);
		} catch (const std::exception& exception) {
			error = std::string("WIT JSON: ") + exception.what();
		}
		std::filesystem::remove(jsonPath);
		std::filesystem::remove(logPath);
		return schema;
	}

	try {
		return witSchema(Json::parse(readText(path)), direction, moduleName,
				worldName, error, asyncPolicy, packageName);
	} catch (const std::exception& exception) {
		error = std::string("WIT JSON: ") + exception.what();
		return {};
	}
}

} // namespace WASM::Bindings

