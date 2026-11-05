#include "WitFrontend.hpp"

#include "BindingSchema.hpp"

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

std::string typeSpelling(const Json& type)
{
	if (type.is_string())
		return type.get<std::string>();
	if (!type.is_object())
		return type.dump();
	if (type.contains("type") && type.at("type").is_string())
		return type.at("type").get<std::string>();
	if (type.contains("name") && type.at("name").is_string())
		return type.at("name").get<std::string>();
	return type.dump();
}

Json schemaTypeForWit(const std::string& spelling)
{
	const std::string type = spelling;
	for (const char* scalar : { "bool", "u8", "s8", "u16", "s16", "u32",
			"s32", "u64", "s64", "f32", "f64", "char", "string",
			"error-context" })
		if (type == scalar)
			return Json{ { "type", type } };
	if (type == "unit")
		return Json{ { "type", "void" } };

	// Structured WIT values (records, variants, lists, resources, handles,
	// streams and futures) do not have a core scalar spelling. Keep them
	// opaque and preserve the WIT spelling as tooling metadata.
	return Json{ { "type", "ref" }, { "heap", "extern" },
		{ "nullable", true }, { "name", type } };
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

void appendFunction(const Json& function, const std::string& fallbackName,
		bool import, const std::string& module, Schema& schema)
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
			const Json type = schemaTypeForWit(
					typeSpelling(parameter.value("type", Json{})));
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
			const Json type = schemaTypeForWit(typeSpelling(result));
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
	if (import)
		entry.raw["module"] = entry.module;
	if (import)
		schema.imports.push_back(std::move(entry));
	else
		schema.exports.push_back(std::move(entry));
}

void appendInterface(const Json& interfaceEntry, bool import,
		const std::string& fallbackModule, Schema& schema)
{
	const std::string interfaceName = stringAt(interfaceEntry, "name");
	const std::string module = import
			? (interfaceName.empty() ? fallbackModule : interfaceName)
			: std::string();
	const auto functions = interfaceEntry.find("functions");
	if (functions == interfaceEntry.end() || !functions->is_object())
		return;
	for (const auto& [name, function] : functions->items())
		appendFunction(function, name, import, module, schema);
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

void appendWorldEntry(const Json& node, const std::string& key,
		bool import, const std::string& moduleName,
		std::map<int, const Json*>& interfaces, Schema& schema, std::string& error)
{
	if (node.contains("function")) {
		appendFunction(node.at("function"), key, import,
				import ? moduleName : std::string(), schema);
		return;
	}
	if (const Json* interfaceEntry = resolveInterface(node, interfaces, error)) {
		appendInterface(*interfaceEntry, import, key, schema);
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
Schema witSchema(const Json& document, Direction direction,
		std::string_view moduleName, std::string_view worldName,
		std::string& error)
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

	const Json* selected = nullptr;
	for (const Json& world : *worlds) {
		if (worldName.empty() || stringAt(world, "name") == worldName) {
			selected = &world;
			break;
		}
	}
	if (selected == nullptr) {
		error = "WIT JSON: requested world was not found";
		return {};
	}

	std::string schemaName(moduleName);
	if (schemaName.empty()) {
		if (document.contains("packages") && document.at("packages").is_array()
				&& !document.at("packages").empty())
			schemaName = stringAt(document.at("packages").at(0), "name");
		if (schemaName.empty())
			schemaName = stringAt(*selected, "name");
		if (schemaName.empty())
			schemaName = "component";
	}

	std::map<int, const Json*> interfaces;
	if (document.contains("interfaces") && document.at("interfaces").is_array()) {
		for (std::size_t i = 0; i < document.at("interfaces").size(); ++i)
			interfaces.emplace(static_cast<int>(i), &document.at("interfaces")[i]);
	}

	Schema schema;
	schema.module = schemaName;
	schema.direction = Direction::Complete;
	schema.extras = { { "kind", "component" }, { "frontend", "wit" },
		{ "world", stringAt(*selected, "name") } };
	if (document.contains("types"))
		schema.types = document.at("types");

	if (selected->contains("imports") && selected->at("imports").is_object()) {
		for (const auto& [name, node] : selected->at("imports").items())
			appendWorldEntry(node, name, true, schemaName, interfaces,
					schema, error);
	}
	if (selected->contains("exports") && selected->at("exports").is_object()) {
		for (const auto& [name, node] : selected->at("exports").items())
			appendWorldEntry(node, name, false, schemaName, interfaces,
					schema, error);
	}
	if (!error.empty())
		return {};
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
		std::string& error)
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
					moduleName, worldName, error);
		} catch (const std::exception& exception) {
			error = std::string("WIT JSON: ") + exception.what();
		}
		std::filesystem::remove(jsonPath);
		std::filesystem::remove(logPath);
		return schema;
	}

	try {
		return witSchema(Json::parse(readText(path)), direction, moduleName,
				worldName, error);
	} catch (const std::exception& exception) {
		error = std::string("WIT JSON: ") + exception.what();
		return {};
	}
}

} // namespace WASM::Bindings

