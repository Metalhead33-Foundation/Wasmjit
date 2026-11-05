#include "CppFrontend.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace WASM::Bindings {

namespace {

std::string readTextFile(const std::filesystem::path& path)
{
	std::ifstream input(path);
	std::ostringstream text;
	text << input.rdbuf();
	return text.str();
}

std::string trim(std::string text)
{
	const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
	text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
	text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(),
			text.end());
	return text;
}

std::string cleanCommentLine(std::string line)
{
	line = trim(std::move(line));
	for (const std::string_view prefix : { "///", "//!", "//", "/**", "/*!",
			"/*", "*", "*/" }) {
		if (line.rfind(prefix, 0) == 0) {
			line.erase(0, prefix.size());
			line = trim(std::move(line));
			break;
		}
	}
	if (line.size() >= 2 && line.compare(line.size() - 2, 2, "*/") == 0)
		line.erase(line.size() - 2);
	line = trim(std::move(line));
	for (const std::string_view command : { "\\brief", "\\return", "\\details" }) {
		if (line.rfind(command, 0) == 0) {
			line.erase(0, command.size());
			line = trim(std::move(line));
			break;
		}
	}
	return line;
}

//! Reads the comment block immediately above a function declaration. SWIG's
//! XML output does not retain C++ comments, so the source is the only place
//! the editorial description can come from.
std::string commentForFunction(const std::string& source, const std::string& name)
{
	std::vector<std::string> lines;
	std::istringstream input(source);
	std::string line;
	while (std::getline(input, line))
		lines.push_back(line);

	const std::string needle = name + "(";
	for (std::size_t i = 0; i < lines.size(); ++i) {
		const std::string trimmed = trim(lines[i]);
		if (trimmed.rfind("//", 0) == 0 || trimmed.rfind("*", 0) == 0
				|| trimmed.rfind("/*", 0) == 0)
			continue;
		const std::size_t found = lines[i].find(needle);
		if (found == std::string::npos)
			continue;
		std::size_t cursor = found + name.size();
		while (cursor < lines[i].size()
				&& std::isspace(static_cast<unsigned char>(lines[i][cursor])))
			++cursor;
		if (cursor >= lines[i].size() || lines[i][cursor] != '(')
			continue;

		std::vector<std::string> collected;
		std::size_t j = i;
		while (j > 0) {
			const std::string previous = trim(lines[j - 1]);
			if (previous.empty())
				break;
			if (previous.rfind("//", 0) == 0) {
				collected.push_back(cleanCommentLine(previous));
				--j;
				continue;
			}
			if (previous.rfind("*/", 0) == 0) {
				std::vector<std::string> block;
				std::size_t k = j;
				while (k > 0) {
					const std::string current = trim(lines[k - 1]);
					const std::string cleaned = cleanCommentLine(current);
					if (!cleaned.empty())
						block.push_back(cleaned);
					--k;
					if (current.rfind("/*", 0) == 0)
						break;
				}
				for (auto it = block.rbegin(); it != block.rend(); ++it)
					collected.push_back(*it);
				break;
			}
			if (previous.rfind("/*", 0) == 0
					|| (previous.find("/*") != std::string::npos
							&& previous.find("*/") != std::string::npos)) {
				collected.push_back(cleanCommentLine(previous));
				break;
			}
			break;
		}
		std::string description;
		for (auto it = collected.rbegin(); it != collected.rend(); ++it) {
			if (it->empty())
				continue;
			if (!description.empty())
				description += " ";
			description += *it;
		}
		return description;
	}
	return {};
}

//! Recovers the declared parameter names from a C++ signature. Glue's
//! declaration printer emits generic `argN` names for some registrations, so
//! the source declaration is the fallback that keeps `argNames` meaningful.
std::vector<std::string> sourceParameterNames(const std::string& source,
		const std::string& name)
{
	std::vector<std::string> names;
	const std::string needle = name + "(";
	std::size_t found = std::string::npos;
	std::size_t search = 0;
	while ((search = source.find(needle, search)) != std::string::npos) {
		const std::size_t lineStart = source.rfind('\n', search);
		const std::string line = source.substr(
				lineStart == std::string::npos ? 0 : lineStart + 1,
				search - (lineStart == std::string::npos ? 0 : lineStart + 1));
		if (trim(line).rfind("//", 0) != 0 && trim(line).rfind("*", 0) != 0
				&& trim(line).rfind("/*", 0) != 0) {
			found = search;
			break;
		}
		search += needle.size();
	}
	if (found == std::string::npos)
		return names;

	std::size_t open = found + name.size();
	while (open < source.size() && source[open] != '(')
		++open;
	int depth = 0;
	std::size_t close = open;
	for (; close < source.size(); ++close) {
		if (source[close] == '(')
			++depth;
		else if (source[close] == ')' && --depth == 0)
			break;
	}
	if (close >= source.size())
		return names;
	const std::string parameters = source.substr(open + 1, close - open - 1);
	std::size_t start = 0;
	int nested = 0;
	for (std::size_t i = 0; i <= parameters.size(); ++i) {
		const bool separator = i == parameters.size()
				|| (parameters[i] == ',' && nested == 0);
		if (i < parameters.size() && (parameters[i] == '<'
				|| parameters[i] == '(' || parameters[i] == '['))
			++nested;
		else if (i < parameters.size() && (parameters[i] == '>'
				|| parameters[i] == ')' || parameters[i] == ']'))
			--nested;
		if (!separator)
			continue;
		std::string parameter = trim(parameters.substr(start, i - start));
		start = i + 1;
		const std::size_t comment = parameter.find("/*");
		if (comment != std::string::npos)
			parameter.erase(comment);
		const std::size_t equal = parameter.find('=');
		if (equal != std::string::npos)
			parameter.erase(equal);
		parameter = trim(std::move(parameter));
		std::size_t end = parameter.size();
		while (end > 0 && std::isspace(static_cast<unsigned char>(parameter[end - 1])))
			--end;
		if (end == 0 || parameter[end - 1] == '*' || parameter[end - 1] == '&') {
			names.emplace_back();
			continue;
		}
		std::size_t begin = end;
		while (begin > 0
				&& (std::isalnum(static_cast<unsigned char>(parameter[begin - 1]))
						|| parameter[begin - 1] == '_'))
			--begin;
		const std::string token = parameter.substr(begin, end - begin);
		const bool hasType = begin > 0
				&& std::isspace(static_cast<unsigned char>(parameter[begin - 1]));
		names.push_back(hasType ? token : std::string());
	}
	return names;
}

std::string decodeXml(std::string text)
{
	const auto replaceAll = [&](std::string_view from, std::string_view to) {
		std::size_t position = 0;
		while ((position = text.find(from, position)) != std::string::npos) {
			text.replace(position, from.size(), to);
			position += to.size();
		}
	};
	replaceAll("&amp;", "&");
	replaceAll("&lt;", "<");
	replaceAll("&gt;", ">");
	replaceAll("&quot;", "\"");
	replaceAll("&apos;", "'");
	replaceAll("&#10;", "\n");
	replaceAll("&#9;", "\t");
	return text;
}

std::optional<std::string> attributeValue(std::string_view line,
		std::string_view attribute)
{
	const std::string needle = "name=\"" + std::string(attribute) + "\"";
	const std::size_t name = line.find(needle);
	if (name == std::string_view::npos)
		return std::nullopt;
	const std::size_t value = line.find("value=\"", name + needle.size());
	if (value == std::string_view::npos)
		return std::nullopt;
	const std::size_t begin = value + 7;
	const std::size_t end = line.find('"', begin);
	if (end == std::string_view::npos)
		return std::nullopt;
	return decodeXml(std::string(line.substr(begin, end - begin)));
}

std::string normalizeCppType(std::string type)
{
	const auto removeAll = [&](std::string_view token) {
		std::size_t position = 0;
		while ((position = type.find(token, position)) != std::string::npos)
			type.erase(position, token.size());
	};
	for (const std::string_view token : { "const", "volatile", "struct", "class",
			"std::", "::", "&", "*", " " })
		removeAll(token);
	return type;
}

bool typeMatches(const std::string& candidateType, const nlohmann::json& wasmType)
{
	std::string kind;
	if (wasmType.is_string())
		kind = wasmType.get<std::string>();
	else if (wasmType.is_object())
		kind = wasmType.value("type", wasmType.value("kind", std::string()));

	const bool pointer = candidateType.find('*') != std::string::npos
			|| candidateType.find('&') != std::string::npos;
	if (kind.find("ref") != std::string::npos || kind == "any"
			|| kind == "extern" || kind == "exn" || kind == "cont")
		return pointer || candidateType == "string";

	// Glue's DeclarationPrinter writes TypeScript spellings.
	if (candidateType == "number")
		return kind == "i32" || kind == "i64" || kind == "f32" || kind == "f64";
	if (candidateType == "bigint")
		return kind == "i64";
	if (candidateType == "boolean")
		return kind == "i32" || kind == "i64";

	const std::string type = normalizeCppType(candidateType);
	if (kind == "i32")
		return type == "int32_t" || type == "int32" || type == "int"
				|| type == "signed" || type == "signedint"
				|| type == "uint32_t" || type == "uint32" || type == "unsigned"
				|| type == "unsignedint";
	if (kind == "i64")
		return type == "int64_t" || type == "int64" || type == "longlong"
				|| type == "long" || type == "uint64_t" || type == "uint64"
				|| type == "unsignedlong" || type == "unsignedlonglong"
				|| type == "size_t" || type == "intptr_t" || type == "uintptr_t";
	if (kind == "f32")
		return type == "float";
	if (kind == "f64")
		return type == "double";
	return false;
}

nlohmann::ordered_json describeCandidate(const CandidateSignature& candidate)
{
	nlohmann::ordered_json entry;
	entry["name"] = candidate.name;
	entry["returnType"] = candidate.returnType;
	entry["params"] = nlohmann::ordered_json::array();
	for (const CandidateParameter& param : candidate.params)
		entry["params"].push_back({ { "type", param.type }, { "name", param.name } });
	return entry;
}

std::vector<CandidateSignature> parseGlueDeclarations(const std::string& text)
{
	std::vector<CandidateSignature> candidates;
	std::istringstream input(text);
	std::string line;
	const std::string prefix = "declare const ";
	while (std::getline(input, line)) {
		line = trim(line);
		if (line.rfind(prefix, 0) != 0)
			continue;
		const std::size_t colon = line.find(':', prefix.size());
		const std::size_t arrow = line.find("=>", colon == std::string::npos
				? prefix.size() : colon);
		const std::size_t open = line.find('(', prefix.size());
		const std::size_t close = arrow == std::string::npos ? std::string::npos
				: line.rfind(')', arrow);
		if (colon == std::string::npos || arrow == std::string::npos
				|| open == std::string::npos || close == std::string::npos
				|| open > close)
			continue;
		CandidateSignature candidate;
		candidate.name = trim(line.substr(prefix.size(),
				colon - prefix.size()));
		candidate.returnType = trim(line.substr(arrow + 2));
		const std::string params = line.substr(open + 1, close - open - 1);
		std::istringstream paramStream(params);
		std::string param;
		std::size_t index = 0;
		while (std::getline(paramStream, param, ',')) {
			param = trim(param);
			if (param.empty() || param.rfind("this:", 0) == 0)
				continue;
			const std::size_t separator = param.find(':');
			if (separator == std::string::npos)
				continue;
			CandidateParameter parsed;
			parsed.name = trim(param.substr(0, separator));
			parsed.type = trim(param.substr(separator + 1));
			if (parsed.name.empty())
				parsed.name = "arg" + std::to_string(index);
			++index;
			candidate.params.push_back(std::move(parsed));
		}
		if (!candidate.name.empty())
			candidates.push_back(std::move(candidate));
	}
	return candidates;
}

} // namespace

std::vector<CandidateSignature> swigCandidates(const std::filesystem::path& header,
		std::string& error)
{
	error.clear();
	if (!std::filesystem::is_regular_file(header)) {
		error = "not a regular file: " + header.string();
		return {};
	}
	const std::string headerPath = std::filesystem::absolute(header).string();
	const auto stamp = std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count());
	const std::filesystem::path xmlPath = std::filesystem::temp_directory_path()
			/ ("wasmjit-candidates-" + stamp + ".xml");
	const std::filesystem::path logPath = std::filesystem::temp_directory_path()
			/ ("wasmjit-candidates-" + stamp + ".log");
	const std::string command = "swig -xml -module wasmjit_candidates -o '"
			+ xmlPath.string() + "' '" + headerPath + "' > '" + logPath.string()
			+ "' 2>&1";
	const int status = std::system(command.c_str());
	if (status != 0) {
		std::ifstream log(logPath);
		std::ostringstream text;
		text << log.rdbuf();
		error = "swig failed: " + text.str();
		std::filesystem::remove(xmlPath);
		std::filesystem::remove(logPath);
		return {};
	}

	std::ifstream xml(xmlPath);
	if (!xml) {
		error = "cannot read swig output: " + xmlPath.string();
		std::filesystem::remove(xmlPath);
		std::filesystem::remove(logPath);
		return {};
	}

	std::vector<CandidateSignature> candidates;
	std::vector<std::string> includes;
	std::string line;
	bool pendingInclude = false;
	bool inCdecl = false;
	bool inParm = false;
	bool sawParmList = false;
	bool isFunction = false;
	CandidateSignature current;
	CandidateParameter currentParam;

	const auto finishCdecl = [&]() {
		if (isFunction && !current.name.empty() && !current.returnType.empty())
			candidates.push_back(current);
		inCdecl = false;
		inParm = false;
		sawParmList = false;
		isFunction = false;
		current = CandidateSignature{};
		currentParam = CandidateParameter{};
	};

	while (std::getline(xml, line)) {
		if (line.find("<include ") != std::string::npos) {
			includes.emplace_back();
			pendingInclude = true;
			continue;
		}
		if (line.find("</include>") != std::string::npos) {
			if (!includes.empty())
				includes.pop_back();
			pendingInclude = false;
			continue;
		}
		if (pendingInclude) {
			if (const auto name = attributeValue(line, "name")) {
				includes.back() = *name;
				pendingInclude = false;
			}
			continue;
		}
		const std::string currentInclude = includes.empty() ? std::string()
				: includes.back();
		if (currentInclude != headerPath)
			continue;

		if (line.find("<cdecl ") != std::string::npos) {
			inCdecl = true;
			inParm = false;
			sawParmList = false;
			isFunction = false;
			current = CandidateSignature{};
			currentParam = CandidateParameter{};
			continue;
		}
		if (!inCdecl)
			continue;
		if (line.find("</cdecl>") != std::string::npos) {
			finishCdecl();
			continue;
		}
		if (line.find("<parmlist") != std::string::npos) {
			sawParmList = true;
			continue;
		}
		if (line.find("<parm ") != std::string::npos) {
			inParm = true;
			currentParam = CandidateParameter{};
			continue;
		}
		if (line.find("</parm>") != std::string::npos) {
			if (!currentParam.type.empty())
				current.params.push_back(currentParam);
			inParm = false;
			currentParam = CandidateParameter{};
			continue;
		}
		if (const auto kind = attributeValue(line, "kind")) {
			if (*kind == "function")
				isFunction = true;
			continue;
		}
		if (const auto type = attributeValue(line, "type")) {
			if (inParm)
				currentParam.type = *type;
			else if (!sawParmList)
				current.returnType = *type;
			continue;
		}
		if (const auto name = attributeValue(line, "name")) {
			if (inParm)
				currentParam.name = *name;
			else
				current.name = *name;
		}
	}
	const std::string source = readTextFile(header);
	for (CandidateSignature& candidate : candidates)
		candidate.description = commentForFunction(source, candidate.name);
	std::filesystem::remove(xmlPath);
	std::filesystem::remove(logPath);
	return candidates;
}

std::vector<CandidateSignature> glueCandidates(const std::filesystem::path& source,
		std::string& error)
{
	error.clear();
	if (!std::filesystem::is_regular_file(source)) {
		error = "not a regular file: " + source.string();
		return {};
	}
	const char* configured = std::getenv("GLUE_PREFIX");
	const std::filesystem::path prefix = configured == nullptr
			? std::filesystem::path("/usr/local") : std::filesystem::path(configured);
	const std::filesystem::path library = prefix / "lib" / "Glue-1.5" / "libGlue.a";
	if (!std::filesystem::is_regular_file(library)) {
		error = "Glue library not found: " + library.string();
		return {};
	}

	const auto stamp = std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count());
	const std::filesystem::path dir = std::filesystem::temp_directory_path()
			/ ("wasmjit-glue-" + stamp);
	std::filesystem::create_directories(dir);
	const std::filesystem::path driver = dir / "driver.cpp";
	const std::filesystem::path executable = dir / "driver";
	const std::filesystem::path declarations = dir / "declarations.txt";
	const std::filesystem::path compileLog = dir / "compile.log";
	const std::filesystem::path runLog = dir / "run.log";
	{
		std::ofstream out(driver, std::ios::binary);
		out << "#include <glue/declarations.h>\n"
			   "#include <glue/context.h>\n"
			   "#include <iostream>\n\n"
			   "glue::MapValue wasmjit_bindings();\n\n"
			   "int main()\n"
			   "{\n"
			   "\tauto map = wasmjit_bindings();\n"
			   "\tglue::Context context;\n"
			   "\tcontext.addRootMap(map);\n"
			   "\tglue::DeclarationPrinter printer;\n"
			   "\tprinter.init();\n"
			   "\tprinter.print(std::cout, map, &context);\n"
			   "}\n";
	}

	std::string compile = "g++ -std=c++2a";
	for (const char* directory : { "Glue-1.5", "Revisited-2.3",
			"StaticTypeInfo-1.2", "EasyIterator-1.4" })
		compile += " -I\"" + (prefix / "include" / directory).string() + "\"";
	compile += " -I\"" + std::filesystem::absolute(source).parent_path().string()
			+ "\" \"" + driver.string() + "\" \""
			+ std::filesystem::absolute(source).string() + "\" -L\""
			+ library.parent_path().string() + "\" -lGlue -o \""
			+ executable.string() + "\" > \"" + compileLog.string() + "\" 2>&1";
	if (std::system(compile.c_str()) != 0) {
		error = "Glue driver compile failed: " + readTextFile(compileLog);
		std::filesystem::remove_all(dir);
		return {};
	}

	const std::string run = "\"" + executable.string() + "\" > \""
			+ declarations.string() + "\" 2> \"" + runLog.string() + "\"";
	if (std::system(run.c_str()) != 0) {
		error = "Glue driver failed: " + readTextFile(runLog);
		std::filesystem::remove_all(dir);
		return {};
	}
	const std::string text = readTextFile(declarations);
	std::vector<CandidateSignature> candidates = parseGlueDeclarations(text);
	const std::string sourceText = readTextFile(source);
	for (CandidateSignature& candidate : candidates) {
		candidate.description = commentForFunction(sourceText, candidate.name);
		const auto names = sourceParameterNames(sourceText, candidate.name);
		for (std::size_t i = 0; i < candidate.params.size() && i < names.size(); ++i) {
			if (!names[i].empty()
					&& (candidate.params[i].name.empty()
							|| candidate.params[i].name.rfind("arg", 0) == 0))
				candidate.params[i].name = names[i];
		}
	}
	std::filesystem::remove_all(dir);
	return candidates;
}


namespace {

bool isVmContext(const std::string& type)
{
	const bool swigPointer = type.rfind("p.", 0) == 0 || type.rfind("r.", 0) == 0;
	return type.find("VMContext") != std::string::npos
			&& (swigPointer || type.find('*') != std::string::npos
					|| type.find('&') != std::string::npos);
}

std::optional<std::string> coreTypeFor(const std::string& type)
{
	std::string base = type;
	const std::size_t comment = base.find("/*");
	if (comment != std::string::npos)
		base.erase(comment);
	base = trim(base);
	const bool swigPointer = base.rfind("p.", 0) == 0 || base.rfind("r.", 0) == 0;
	if (swigPointer)
		base.erase(0, 2);
	if (swigPointer)
		return "externref";
	const std::string normalized = normalizeCppType(base);
	if (normalized == "int8_t" || normalized == "uint8_t"
			|| normalized == "int16_t" || normalized == "uint16_t"
			|| normalized == "int32_t" || normalized == "uint32_t"
			|| normalized == "int32" || normalized == "uint32"
			|| normalized == "int" || normalized == "unsigned"
			|| normalized == "signed" || normalized == "short"
			|| normalized == "unsignedshort" || normalized == "char"
			|| normalized == "signedchar" || normalized == "unsignedchar"
			|| normalized == "bool" || normalized == "boolean"
			|| normalized == "number")
		return "i32";
	if (normalized == "int64_t" || normalized == "uint64_t"
			|| normalized == "int64" || normalized == "uint64"
			|| normalized == "long" || normalized == "unsignedlong"
			|| normalized == "longlong" || normalized == "unsignedlonglong"
			|| normalized == "size_t" || normalized == "intptr_t"
			|| normalized == "uintptr_t" || normalized == "bigint")
		return "i64";
	if (normalized == "float")
		return "f32";
	if (normalized == "double")
		return "f64";
	if (normalized == "v128")
		return "v128";
	if (base.find('*') != std::string::npos
			|| base.find('&') != std::string::npos
			|| normalized == "unknown" || normalized == "string"
			|| normalized == "char" || normalized == "void")
		return "externref";
	return std::nullopt;
}

} // namespace

Schema candidateSchemaModel(
		const std::vector<CandidateSignature>& candidates,
		Direction direction, std::string_view moduleName,
		std::string_view frontend)
{
	if (direction != Direction::GuestToHost && direction != Direction::Complete)
		throw std::runtime_error(
				"C++ host candidates describe Wasm imports; use --direction g2h or all");

	Schema schema;
	schema.direction = direction;
	schema.module = std::string(moduleName);
	schema.frontend = std::string(frontend);
	schema.extras = { { "kind", "module" }, { "frontend", schema.frontend } };

	std::set<std::string> emittedNames;
	for (const CandidateSignature& candidate : candidates) {
		if (candidate.params.empty() || !isVmContext(candidate.params[0].type))
			continue;
		Entry entry;
		entry.kind = EntryKind::Function;
		entry.import = true;
		entry.module = std::string(moduleName);
		entry.name = candidate.name;
		entry.description = candidate.description;
		bool fits = true;
		for (std::size_t i = 1; i < candidate.params.size(); ++i) {
			const auto type = coreTypeFor(candidate.params[i].type);
			if (!type) {
				fits = false;
				break;
			}
			entry.function.params.push_back(ValueType::fromJson(
					nlohmann::ordered_json{ { "type", *type } }));
			const std::string& name = candidate.params[i].name;
			entry.argNames.push_back(
					name.empty() ? "arg" + std::to_string(i - 1) : name);
		}
		if (!fits)
			continue;
		if (candidate.returnType != "void") {
			const auto type = coreTypeFor(candidate.returnType);
			if (!type)
				continue;
			entry.function.results.push_back(ValueType::fromJson(
					nlohmann::ordered_json{ { "type", *type } }));
		}
		if (!emittedNames.insert(candidate.name).second)
			continue;
		nlohmann::ordered_json type = { { "kind", "func" } };
		type["params"] = nlohmann::ordered_json::array();
		for (const ValueType& param : entry.function.params)
			type["params"].push_back(param.toJson());
		type["results"] = nlohmann::ordered_json::array();
		for (const ValueType& result : entry.function.results)
			type["results"].push_back(result.toJson());
		entry.raw = {
			{ "kind", "function" },
			{ "module", entry.module },
			{ "name", entry.name },
			{ "description", entry.description },
			{ "argNames", entry.argNames },
			{ "type", std::move(type) },
		};
		schema.imports.push_back(std::move(entry));
	}
	schema.dedupe();
	return schema;
}

nlohmann::ordered_json candidateSchema(
		const std::vector<CandidateSignature>& candidates,
		std::string_view direction, std::string_view moduleName,
		std::string_view frontend)
{
	Direction parsed = Direction::GuestToHost;
	if (!parseDirection(direction, parsed))
		throw std::runtime_error("unknown candidate schema direction: "
				+ std::string(direction));
	return candidateSchemaModel(candidates, parsed, moduleName, frontend)
			.toJson();
}

nlohmann::ordered_json crossCheckCandidates(const nlohmann::ordered_json& schema,
		const std::vector<CandidateSignature>& candidates, std::string_view frontend)
{
	nlohmann::ordered_json report;
	report["frontend"] = std::string(frontend);
	report["schema"] = schema.value("schema", std::string());
	report["candidates"] = nlohmann::ordered_json::array();
	std::size_t matchedCount = 0;

	for (const CandidateSignature& candidate : candidates) {
		nlohmann::ordered_json entry = describeCandidate(candidate);
		const nlohmann::ordered_json* function = nullptr;
		for (const char* section : { "exports", "imports" }) {
			const auto found = schema.find(section);
			if (found == schema.end() || !found->is_array())
				continue;
			for (const nlohmann::ordered_json& item : *found) {
				if (item.value("kind", std::string()) == "function"
						&& item.value("name", std::string()) == candidate.name) {
					function = &item;
					break;
				}
			}
			if (function != nullptr)
				break;
		}

		bool matched = false;
		std::string reason;
		if (function == nullptr) {
			reason = "no schema function with this name";
		} else {
			const nlohmann::json& type = function->at("type");
			const nlohmann::json params = type.value("params", nlohmann::json::array());
			const nlohmann::json results = type.value("results", nlohmann::json::array());
			if (params.size() != candidate.params.size()) {
				reason = "parameter count differs from the schema";
			} else if (results.size() > 1u) {
				reason = "the schema has multiple results; a C++ candidate returns one value";
			} else if (results.empty() && candidate.returnType != "void") {
				reason = "the schema returns nothing";
			} else if (!results.empty() && candidate.returnType == "void") {
				reason = "the schema returns a value";
			} else if (!results.empty()
					&& !typeMatches(candidate.returnType, results[0])) {
				reason = "return type does not match the schema";
			} else {
				matched = true;
				for (std::size_t i = 0; i < candidate.params.size(); ++i) {
					if (!typeMatches(candidate.params[i].type, params[i])) {
						matched = false;
						reason = "parameter " + std::to_string(i)
								+ " type does not match the schema";
						break;
					}
				}
			}
		}
		entry["schema"] = function == nullptr ? nlohmann::json(nullptr)
				: nlohmann::json(function->value("name", std::string()));
		entry["matched"] = matched;
		entry["reason"] = reason;
		if (matched)
			++matchedCount;
		report["candidates"].push_back(std::move(entry));
	}
	report["matched"] = matchedCount;
	report["unmatched"] = candidates.size() - matchedCount;
	return report;
}

} // namespace WASM::Bindings
