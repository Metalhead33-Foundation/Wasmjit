#include "CppFrontend.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace WASM::Bindings {

namespace {

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
		return pointer;

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
	std::filesystem::remove(xmlPath);
	std::filesystem::remove(logPath);
	return candidates;
}

nlohmann::ordered_json crossCheckCandidates(const nlohmann::ordered_json& schema,
		const std::vector<CandidateSignature>& candidates)
{
	nlohmann::ordered_json report;
	report["frontend"] = "swig";
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
