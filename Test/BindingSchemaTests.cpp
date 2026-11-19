#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <Euphemy/Io/EuphFile.hpp>

#include "BindingSchema.hpp"
#include "frontend/cpp/CppFrontend.hpp"
#include "frontend/wit/WitFrontend.hpp"
#include "helper.hpp"
#include "TestStore.hpp"
#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmStore.hpp"

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

#ifndef WASM_TESTSUITE_SOURCE_DIR
#define WASM_TESTSUITE_SOURCE_DIR ""
#endif

// docs/EMBEDDING_SURFACE.md §11.8: conformance tests for the first-party
// bindingSchema tool. The implementation lives in Tools/Bindings/, outside
// WasmBase/; these tests prove the schema and both emitters from the engine's
// public descriptors.
namespace {

using Json = nlohmann::ordered_json;

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

const Json* findByName(const Json& array, std::string_view name)
{
	for (const Json& entry : array) {
		if (entry.value("name", "") == name)
			return &entry;
	}
	return nullptr;
}

// A `typeId` is tooling metadata scoped to one Store, so two parses of the
// same module assign different numbers. Structural comparison ignores it.
Json withoutTypeIds(Json value)
{
	if (value.is_object()) {
		value.erase("typeId");
		for (auto& [key, item] : value.items()) {
			(void)key;
			item = withoutTypeIds(item);
		}
	} else if (value.is_array()) {
		for (auto& item : value)
			item = withoutTypeIds(item);
	}
	return value;
}

void loadTestComponent(const char* name, WASM::Component& component)
{
	const std::string path = std::string(WASM_TEST_DIR) + name + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	component.fromFile(file, testStore());
	component.validate();
}

// A hand-built `(func (export "echo") (param stringref) (result stringref))`
// using the engine's reserved stringref value-type byte 0x67
// (docs/proposals/STRINGREF.md STR-1). WAT cannot be used here because the
// upstream proposal's current encoding collides with GC `ref`/`ref null`.
std::vector<uint8_t> stringRefModuleBytes()
{
	return {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x06, 0x01, 0x60, 0x01, 0x67, 0x01, 0x67,
		0x03, 0x02, 0x01, 0x00,
		0x07, 0x08, 0x01, 0x04, 'e', 'c', 'h', 'o', 0x00, 0x00,
		0x0a, 0x06, 0x01, 0x04, 0x00, 0x20, 0x00, 0x0b
	};
}

std::string readText(const std::filesystem::path& path);
//! Sanitizer flags a generated child program must inherit from this binary.
std::string childSanitizerFlags();
bool haveTool(const char* probe);
std::filesystem::path bindingsDir();

void runCppAdapterConformance(const char* moduleName, const std::string& mainSource,
		const char* directoryName)
{
	if (!haveTool("command -v g++ > /dev/null 2>&1")) {
		WARN("g++ not found; skipped the generated C++ adapter conformance check");
		return;
	}

	const std::filesystem::path dir = bindingsDir() / directoryName;
	std::filesystem::create_directories(dir);
	const WASM::Module module = loadTestModule(moduleName);
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, moduleName);

	const auto write = [&](std::string_view name, const std::string& text) {
		const std::filesystem::path path = dir / std::string(name);
		std::ofstream out(path, std::ios::binary);
		out << text;
		return path;
	};
	write("adapter.h", WASM::Bindings::emit(schema, WASM::Bindings::Language::CHeader));
	write("adapter.cpp",
			WASM::Bindings::emit(schema, WASM::Bindings::Language::CppAdapter));
	write("main.cpp", mainSource);

	const std::filesystem::path sourceRoot =
			std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR).parent_path().parent_path();
	const std::filesystem::path buildRoot = std::filesystem::path(WASM_TEST_DIR)
			.parent_path().parent_path().parent_path();
	const std::string compile = "g++ -std=c++2a" + childSanitizerFlags()
			+ " -I\"" + dir.string() + "\" -I\""
			+ sourceRoot.string() + "\" -I\"" + (sourceRoot / "extern" / "MhLib").string()
			+ "\" \"" + (dir / "main.cpp").string() + "\" \""
			+ (dir / "adapter.cpp").string() + "\" -L\""
			+ (buildRoot / "WasmBase").string() + "\" -L\""
			+ (buildRoot / "LibJit").string() + "\" -L\""
			+ (buildRoot / "extern" / "MhLib" / "Elvavena").string() + "\" -L\""
			+ (buildRoot / "extern" / "MhLib" / "Euphemy").string()
			+ "\" -lWasmBase -lLibJit -lEuphemy -lElvavena -ljit -Wl,-rpath,\""
			+ (buildRoot / "WasmBase").string() + ":" + (buildRoot / "LibJit").string()
			+ ":" + (buildRoot / "extern" / "MhLib" / "Elvavena").string() + ":"
			+ (buildRoot / "extern" / "MhLib" / "Euphemy").string() + "\" -o \""
			+ (dir / "conformance").string() + "\" > \"" + (dir / "compile.log").string()
			+ "\" 2>&1";
	INFO(readText(dir / "compile.log"));
	REQUIRE(std::system(compile.c_str()) == 0);

	const std::string run = "\"" + (dir / "conformance").string() + "\" \""
			+ (std::filesystem::path(WASM_TEST_DIR)
							/ (std::string(moduleName) + ".wasm")).string()
			+ "\" > \"" + (dir / "run.log").string() + "\" 2>&1";
	INFO(readText(dir / "run.log"));
	REQUIRE(std::system(run.c_str()) == 0);
}

// The flags a generated child program needs so that it can link this binary's
// core: when the harness is instrumented the child must be too, because the
// sanitizer runtime has to be the first library in the process.
std::string childSanitizerFlags()
{
#if defined(__SANITIZE_ADDRESS__)
	return " -fsanitize=address -fno-omit-frame-pointer";
#else
	return {};
#endif
}

std::string readText(const std::filesystem::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

bool haveTool(const char* probe)
{
	return std::system(probe) == 0;
}

std::filesystem::path bindingsDir()
{
	const std::filesystem::path dir =
			std::filesystem::path(WASM_TEST_DIR) / "bindings";
	std::filesystem::create_directories(dir);
	return dir;
}

} // namespace

TEST_CASE("the binding schema projects core function and global descriptors", "[bindings]")
{
	const WASM::Module module = loadTestModule("asmjit_i32_basic");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "asmjit_i32_basic");

	REQUIRE(schema.at("schema") == "wasmjit.binding/1");
	REQUIRE(schema.at("direction") == "h2g");
	REQUIRE(schema.at("module") == "asmjit_i32_basic");
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
	const Json importSchema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "wasm_js_string_length");

	const Json* length = findImport(importSchema, "wasm:js-string", "length");
	REQUIRE(length != nullptr);
	REQUIRE(length->at("kind") == "function");
	REQUIRE(length->at("type").at("params").size() == 1u);
	REQUIRE(length->at("type").at("params")[0].at("type") == "ref");
	REQUIRE(length->at("type").at("params")[0].at("heap") == "extern");
	REQUIRE(length->at("type").at("params")[0].at("nullable") == true);
	REQUIRE(length->at("type").at("results")[0].at("type") == "i32");

	const Json exportSchema = WASM::Bindings::moduleSchema(module,
			testStore().types(), WASM::Bindings::Direction::HostToGuest,
			"wasm_js_string_length");
	const Json* identity = findExport(exportSchema, "identity");
	REQUIRE(identity != nullptr);
	const std::string paramType =
			identity->at("type").at("params").at(0).at("type").get<std::string>();
	const std::string resultType =
			identity->at("type").at("results").at(0).at("type").get<std::string>();
	REQUIRE(paramType == "ref");
	REQUIRE(resultType == "ref");

	const std::string header = WASM::Bindings::emitCHeader(exportSchema);
	REQUIRE(header.find("typedef struct wasm_externref wasm_externref;") != std::string::npos);
	REQUIRE(header.find("WASMJIT_WASM_IMPORT") != std::string::npos);
	REQUIRE(header.find("wasm_externref* arg0 /* borrowed */") != std::string::npos);
	REQUIRE(header.find("/* ownership: explicit */") != std::string::npos);
	REQUIRE(header.find("typeId") == std::string::npos);
	REQUIRE(header.find("sizeof(") == std::string::npos);

	const std::string rust = WASM::Bindings::emitRustModule(exportSchema);
	REQUIRE(rust.find("pub struct wasm_externref") != std::string::npos);
	REQUIRE(rust.find("pub type wasmjit_identity_t") != std::string::npos);

	const std::string importRust = WASM::Bindings::emitRustModule(importSchema);
	REQUIRE(importRust.find("pub fn wasmjit_wasm_js_string_length") != std::string::npos);
	REQUIRE(importRust.find("extern \"C\"") != std::string::npos);
	REQUIRE(importRust.find("#[link(wasm_import_module = \"wasm:js-string\")]")
			!= std::string::npos);
	REQUIRE(importRust.find("#[link_name = \"length\"]") != std::string::npos);
}

TEST_CASE("g2h Rust emits Wasm import declarations", "[bindings]")
{
	const WASM::Module module = loadTestModule("asmjit_i32_import");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "asmjit_i32_import");
	const std::string rust = WASM::Bindings::emitRustModule(schema);

	REQUIRE(rust.find("#[link(wasm_import_module = \"host\")]") != std::string::npos);
	REQUIRE(rust.find("#[link_name = \"double\"]") != std::string::npos);
	REQUIRE(rust.find("pub fn wasmjit_host_double(arg0: i32) -> i32;")
			!= std::string::npos);

	const Json both = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::GuestToGuest, "asmjit_i32_import");
	const std::string rustBoth = WASM::Bindings::emitRustModule(both);
	REQUIRE(rustBoth.find("#[link(wasm_import_module = \"asmjit_i32_import\")]")
			!= std::string::npos);
	REQUIRE(rustBoth.find("#[link_name = \"call_host\"]") != std::string::npos);
	REQUIRE(rustBoth.find("#[export_name = \"call_host\"]") == std::string::npos);

	if (haveTool("rustc --version > /dev/null 2>&1")) {
		const std::filesystem::path dir = bindingsDir();
		const std::filesystem::path source = dir / "asmjit_i32_import.rs";
		const std::filesystem::path library = dir / "asmjit_i32_import.rlib";
		const std::filesystem::path log = dir / "rustc-g2h.log";
		{
			std::ofstream out(source, std::ios::binary);
			out << rust;
		}
		const std::string command = "rustc --crate-type lib --edition 2021 \""
				+ source.string() + "\" -o \"" + library.string() + "\" > \""
				+ log.string() + "\" 2>&1";
		INFO(readText(log));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("rustc not found; skipped the g2h Rust compile check");
	}
}

TEST_CASE("a complete schema can be projected to every direction", "[bindings]")
{
	const WASM::Module module = loadTestModule("asmjit_i32_import");
	const Json complete = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::Complete, "asmjit_i32_import");
	REQUIRE(complete.at("direction") == "all");
	REQUIRE(findImport(complete, "host", "double") != nullptr);
	REQUIRE(findExport(complete, "call_host") != nullptr);
	for (const char* section : { "imports", "exports" }) {
		for (const Json& entry : complete.at(section)) {
			if (entry.value("kind", "") == "function") {
				REQUIRE(entry.contains("description"));
				REQUIRE(entry.contains("argNames"));
				REQUIRE(entry.at("argNames").is_array());
			}
		}
	}

	const Json g2h = WASM::Bindings::projectSchema(complete,
			WASM::Bindings::Direction::GuestToHost);
	REQUIRE(g2h.at("direction") == "g2h");
	REQUIRE(findImport(g2h, "host", "double") != nullptr);
	REQUIRE(g2h.at("exports").empty());

	const Json h2g = WASM::Bindings::projectSchema(complete,
			WASM::Bindings::Direction::HostToGuest);
	REQUIRE(h2g.at("direction") == "h2g");
	REQUIRE(h2g.at("imports").empty());
	REQUIRE(findExport(h2g, "call_host") != nullptr);

	const Json g2g = WASM::Bindings::projectSchema(complete,
			WASM::Bindings::Direction::GuestToGuest);
	REQUIRE(g2g.at("imports").size() == 1u);
	REQUIRE(findImport(g2g, "asmjit_i32_import", "call_host") != nullptr);
	REQUIRE(g2g.at("exports").empty());

	Json annotated = complete;
	annotated["imports"][0]["description"] = "Double a value.";
	annotated["imports"][0]["argNames"] = Json::array({"value"});
	annotated["exports"][0]["description"] = "Entry point called from the host.";
	annotated["exports"][0]["argNames"] = Json::array({"seed"});

	const std::string rust = WASM::Bindings::emit(
			WASM::Bindings::projectSchema(annotated,
					WASM::Bindings::Direction::GuestToHost),
			WASM::Bindings::Language::Rust);
	REQUIRE(rust.find("// G2H import: host.double — Double a value.")
			!= std::string::npos);
	REQUIRE(rust.find("pub fn wasmjit_host_double(value: i32)") != std::string::npos);

	const std::string c = WASM::Bindings::emit(
			WASM::Bindings::projectSchema(annotated,
					WASM::Bindings::Direction::HostToGuest),
			WASM::Bindings::Language::CHeader);
	REQUIRE(c.find("/* Entry point called from the host. */") != std::string::npos);
	REQUIRE(c.find("int32_t wasmjit_call_host(int32_t seed);") != std::string::npos);

	REQUIRE_THROWS(WASM::Bindings::projectSchema(g2h,
			WASM::Bindings::Direction::HostToGuest));
}

TEST_CASE("the typed schema model parses enums and round-trips JSON", "[bindings]")
{
	const Json input = {
		{ "schema", "wasmjit.binding/1" },
		{ "direction", "g2h" },
		{ "module", "typed" },
		{ "imports", Json::array({
			{ { "kind", "function" }, { "module", "host" },
				{ "name", "map" }, { "description", "Maps a value." },
				{ "argNames", Json::array({ "value", "handle" }) },
				{ "type", { { "kind", "func" },
					{ "params", Json::array({ { { "type", "i32" } },
						{ { "type", "ref" }, { "heap", "extern" },
							{ "nullable", true } } }) },
					{ "results", Json::array() } } } },
		}) },
		{ "exports", Json::array() },
	};
	const WASM::Bindings::Schema schema = WASM::Bindings::Schema::fromJson(input);
	REQUIRE(schema.direction == WASM::Bindings::Direction::GuestToHost);
	REQUIRE(schema.imports.size() == 1u);
	const WASM::Bindings::Entry& entry = schema.imports[0];
	REQUIRE(entry.kind == WASM::Bindings::EntryKind::Function);
	REQUIRE(entry.function.params.size() == 2u);
	REQUIRE(entry.function.params[0].kind == WASM::Bindings::ValueKind::I32);
	REQUIRE(entry.function.params[1].kind == WASM::Bindings::ValueKind::Ref);
	REQUIRE(entry.function.params[1].heap == WASM::Bindings::HeapKind::Extern);
	REQUIRE(entry.description == "Maps a value.");
	REQUIRE(entry.argNames[1] == "handle");
	const Json output = schema.toJson();
	REQUIRE(output.at("imports")[0].at("name") == "map");
	REQUIRE(output.at("imports")[0].at("description") == "Maps a value.");
}

TEST_CASE("defined references use a kind name, not a numeric TypeId, in the header", "[bindings]")
{
	const WASM::Module module = loadTestModule("plain_ref");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "plain_ref");

	const Json* function = findExport(schema, "f");
	REQUIRE(function != nullptr);
	const Json& param = function->at("type").at("params").at(0);
	REQUIRE(param.at("type").get<std::string>() == "ref");
	REQUIRE(param.at("heap").get<std::string>() == "defined");
	REQUIRE(param.at("typeKind").get<std::string>() == "struct");

	const std::string header = WASM::Bindings::emitCHeader(schema);
	REQUIRE(header.find("typedef struct wasm_structref wasm_structref;") != std::string::npos);
	REQUIRE(header.find("wasm_structref* arg0 /* borrowed */") != std::string::npos);
	REQUIRE(header.find("wasm_ref_") == std::string::npos);

	const std::string rust = WASM::Bindings::emitRustModule(schema);
	REQUIRE(rust.find("pub struct wasm_structref") != std::string::npos);
	REQUIRE(rust.find("arg0: *mut wasm_structref") != std::string::npos);
}

TEST_CASE("exact reference flags are preserved in the schema", "[bindings]")
{
	const WASM::Module module = loadTestModule("exact_ref");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "exact_ref");

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
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::GuestToGuest, "gc_refcount");

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

TEST_CASE("the generated C header and Rust module compile", "[bindings]")
{
	const WASM::Module module = loadTestModule("wasm_js_string_length");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "wasm_js_string_length");
	const std::filesystem::path dir = bindingsDir();

	const std::filesystem::path headerPath = dir / "wasm_js_string_length.h";
	const std::filesystem::path cSourcePath = dir / "wasm_js_string_length.c";
	const std::filesystem::path cppSourcePath = dir / "wasm_js_string_length.cpp";
	const std::filesystem::path rustPath = dir / "wasm_js_string_length.rs";
	const std::filesystem::path cLogPath = dir / "gcc.log";
	const std::filesystem::path cppLogPath = dir / "g++-header.log";
	const std::filesystem::path rustLogPath = dir / "rustc.log";

	const std::string headerText = WASM::Bindings::emitCHeader(schema);
	REQUIRE(headerText.find("#ifdef __cplusplus\nextern \"C\" {\n#endif")
			!= std::string::npos);
	REQUIRE(headerText.find("#ifdef __cplusplus\n}\n#endif")
			!= std::string::npos);
	{
		std::ofstream header(headerPath, std::ios::binary);
		header << headerText;
		std::ofstream source(cSourcePath, std::ios::binary);
		source << "#include \"wasm_js_string_length.h\"\nint main(void) { return 0; }\n";
		std::ofstream cppSource(cppSourcePath, std::ios::binary);
		cppSource << "#include \"wasm_js_string_length.h\"\nint main() { return 0; }\n";
		std::ofstream rust(rustPath, std::ios::binary);
		rust << WASM::Bindings::emitRustModule(schema);
	}

	if (haveTool("gcc --version > /dev/null 2>&1")) {
		const std::string command = "gcc -std=c11 -fsyntax-only -I\"" + dir.string()
				+ "\" \"" + cSourcePath.string() + "\" > \"" + cLogPath.string() + "\" 2>&1";
		INFO(readText(cLogPath));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("gcc not found; skipped the generated C header compile check");
	}

	if (haveTool("g++ --version > /dev/null 2>&1")) {
		const std::string command = "g++ -std=c++2a -fsyntax-only -I\"" + dir.string()
				+ "\" \"" + cppSourcePath.string() + "\" > \"" + cppLogPath.string()
				+ "\" 2>&1";
		INFO(readText(cppLogPath));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("g++ not found; skipped the generated C header C++ check");
	}

	if (haveTool("rustc --version > /dev/null 2>&1")) {
		const std::filesystem::path rustLib = dir / "wasm_js_string_length.rlib";
		const std::string command = "rustc --crate-type lib --edition 2021 \""
				+ rustPath.string() + "\" -o \"" + rustLib.string() + "\" > \""
				+ rustLogPath.string() + "\" 2>&1";
		INFO(readText(rustLogPath));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("rustc not found; skipped the generated Rust compile check");
	}
}

TEST_CASE("an optimized module keeps the same binding signatures", "[bindings]")
{
	if (!haveTool("wasm-opt --version > /dev/null 2>&1")) {
		WARN("wasm-opt not found; skipped the post-optimization schema check");
		return;
	}

	// TB-6/TB-7: pin the optimizer invocation. `--all-features` is required
	// because the fixtures use tail calls, bulk memory, reference types and
	// multi-value; wasm-opt 124 was the host version when this check was added
	// (2026-11-04).
	const std::vector<std::string> fixtures = {
		"asmjit_i32_basic",
		"multi_value",
		"gc_refcount",
		"bindings_ref_ownership",
		"wasm_js_string_length"
	};
	for (const std::string& fixture : fixtures) {
		INFO(fixture);
		const std::filesystem::path input =
				std::filesystem::path(WASM_TEST_DIR) / (fixture + ".wasm");
		const std::filesystem::path output =
				bindingsDir() / (fixture + ".opt.wasm");
		const std::filesystem::path log =
				bindingsDir() / ("wasm-opt-" + fixture + ".log");
		const std::string command = "wasm-opt --all-features -O2 \"" + input.string()
				+ "\" -o \"" + output.string() + "\" > \"" + log.string() + "\" 2>&1";
		INFO(readText(log));
		REQUIRE(std::system(command.c_str()) == 0);

		const Json baseline = WASM::Bindings::moduleSchema(loadTestModule(fixture.c_str()),
				testStore().types(), WASM::Bindings::Direction::GuestToGuest, fixture);

		WASM::Store optimizedStore;
		WASM::Module optimizedModule;
		Euph::Io::File file(output.c_str(), Elv::Io::Mode::READ);
		optimizedModule.fromFile(file, optimizedStore);
		const Json optimized = WASM::Bindings::moduleSchema(optimizedModule,
				optimizedStore.types(), WASM::Bindings::Direction::GuestToGuest,
				fixture);

		// In G2G the producer's exports are the consumer's imports; comparing
		// that one projection covers the exported signatures after optimization.
		for (const Json& original : baseline.at("imports")) {
			if (original.value("kind", "") != "function")
				continue;
			const std::string name = original.at("name").get<std::string>();
			const Json* match = findImport(optimized,
					original.value("module", ""), name);
			REQUIRE(match != nullptr);
			REQUIRE(match->at("kind") == original.at("kind"));
			REQUIRE(withoutTypeIds(match->at("type").at("params"))
					== withoutTypeIds(original.at("type").at("params")));
			REQUIRE(withoutTypeIds(match->at("type").at("results"))
					== withoutTypeIds(original.at("type").at("results")));
		}
	}
}


TEST_CASE("producer toolchain evidence is recorded", "[bindings]")
{
	if (!haveTool("wasm-opt --version > /dev/null 2>&1")
			|| !haveTool("wasm-tools --version > /dev/null 2>&1")) {
		WARN("wasm-opt or wasm-tools not found; skipped the toolchain evidence");
		return;
	}

	const std::filesystem::path path = bindingsDir() / "toolchain.txt";
	{
		std::ofstream out(path, std::ios::binary);
		out << "# Binding producer evidence, 2026-11-04 host.\n"
			   "# wasm-opt flags used by [bindings]: --all-features -O2\n";
	}
	for (const char* command : { "wasm-opt --version", "wasm-tools --version",
			"wat2wasm --version", "clang --version", "rustc --version",
			"go version" }) {
		const std::string run = std::string(command) + " >> \"" + path.string()
				+ "\" 2>&1";
		const int status = std::system(run.c_str());
		(void)status;
	}
	const std::string evidence = readText(path);
	REQUIRE(evidence.find("wasm-opt version") != std::string::npos);
	REQUIRE(evidence.find("wasm-tools") != std::string::npos);
	REQUIRE(evidence.find("--all-features -O2") != std::string::npos);
	INFO(evidence);
}

TEST_CASE("the remaining language emitters produce declarations", "[bindings]")
{
	const WASM::Module module = loadTestModule("wasm_js_string_length");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "wasm_js_string_length");
	const Json exportSchema = WASM::Bindings::moduleSchema(module,
			testStore().types(), WASM::Bindings::Direction::HostToGuest,
			"wasm_js_string_length");
	const std::filesystem::path dir = bindingsDir();

	const auto write = [&](std::string_view name, const std::string& text) {
		const std::filesystem::path path = dir / std::string(name);
		std::ofstream out(path, std::ios::binary);
		out << text;
		return path;
	};
	const auto run = [&](const std::string& command,
							  const std::filesystem::path& log) {
		INFO(readText(log));
		return std::system(command.c_str()) == 0;
	};

	const std::string ts = WASM::Bindings::emit(exportSchema,
			WASM::Bindings::Language::TypeScript);
	REQUIRE(ts.find("export interface WasmExternref") != std::string::npos);
	REQUIRE(ts.find("export declare function wasmjit_identity") != std::string::npos);
	if (haveTool("command -v tsc > /dev/null 2>&1")) {
		const auto path = write("lang.d.ts", ts);
		const auto log = dir / "tsc-lang.log";
		REQUIRE(run("tsc --noEmit --strict \"" + path.string() + "\" > \""
							+ log.string() + "\" 2>&1",
				log));
	} else {
		WARN("tsc not found; skipped the TypeScript check");
	}

	const std::string as = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::AssemblyScript);
	REQUIRE(as.find("@external(\"wasm:js-string\", \"length\")") != std::string::npos);
	REQUIRE(as.find("externref") != std::string::npos);
	const std::filesystem::path asc = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ "node_modules" / ".bin" / "asc";
	if (std::filesystem::exists(asc)) {
		const auto path = write("lang.ts", as);
		const auto log = dir / "asc-lang.log";
		REQUIRE(run("\"" + asc.string() + "\" --noEmit --enable reference-types \""
							+ path.string() + "\" > \"" + log.string() + "\" 2>&1",
				log));
	} else {
		WARN("asc not found; skipped the AssemblyScript check");
	}

	const std::string zig = WASM::Bindings::emit(schema, WASM::Bindings::Language::Zig);
	REQUIRE(zig.find("= @extern(") != std::string::npos);
	REQUIRE(zig.find(".name = \"length\"") != std::string::npos);
	REQUIRE(zig.find(".library_name = \"wasm:js-string\"") != std::string::npos);
	REQUIRE(zig.find("pub const wasm_externref = opaque {};") != std::string::npos);
	if (haveTool("command -v zig > /dev/null 2>&1")) {
		const auto path = write("lang.zig", zig);
		const auto log = dir / "zig-lang.log";
		REQUIRE(run("zig ast-check \"" + path.string() + "\" > \"" + log.string()
							+ "\" 2>&1",
				log));
	} else {
		WARN("zig not found; skipped the Zig check");
	}

	const std::string kotlin = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::Kotlin);
	REQUIRE(kotlin.find("class WasmExternref private constructor()") != std::string::npos);
	REQUIRE(kotlin.find("external fun wasmjit_wasm_js_string_length") != std::string::npos);
	if (haveTool("command -v kotlinc > /dev/null 2>&1")) {
		const auto path = write("lang.kt", kotlin);
		const auto jar = dir / "lang.jar";
		const auto log = dir / "kotlin-lang.log";
		REQUIRE(run("kotlinc \"" + path.string() + "\" -d \"" + jar.string() + "\" > \""
							+ log.string() + "\" 2>&1",
				log));
	} else {
		WARN("kotlinc not found; skipped the Kotlin check");
	}

	const std::string moonbit = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::MoonBit);
	REQUIRE(moonbit.find("#borrow(arg0)") != std::string::npos);
	REQUIRE(moonbit.find("extern \"wasm\" fn wasmjit_wasm_js_string_length")
			!= std::string::npos);
	if (haveTool("command -v moonc > /dev/null 2>&1")) {
		const auto path = write("lang.mbt", moonbit);
		const auto log = dir / "moonbit-lang.log";
		REQUIRE(run("cd \"" + dir.string() + "\" && moonc check \"" + path.string()
							+ "\" > \"" + log.string() + "\" 2>&1",
				log));
	} else {
		WARN("moonc not found; skipped the MoonBit check");
	}

	const std::string go = WASM::Bindings::emit(exportSchema,
			WASM::Bindings::Language::Go);
	const std::string goImport = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::Go);
	REQUIRE(goImport.find("//go:wasmimport wasm:js-string length") != std::string::npos);
	REQUIRE(go.find("//go:wasmexport len") != std::string::npos);
	if (haveTool("command -v gofmt > /dev/null 2>&1")) {
		const auto path = write("lang.go", go);
		const auto log = dir / "go-lang.log";
		REQUIRE(run("gofmt -e \"" + path.string() + "\" > /dev/null 2> \""
							+ log.string() + "\"",
				log));
	} else {
		WARN("gofmt not found; skipped the Go syntax check");
	}
	if (haveTool("command -v go > /dev/null 2>&1")) {
		const std::filesystem::path goDir = dir / "gopkg";
		std::filesystem::create_directories(goDir);
		{
			std::ofstream mod(goDir / "go.mod", std::ios::binary);
			mod << "module example.com/bindings\n\ngo 1.24\n";
			std::ofstream source(goDir / "bindings.go", std::ios::binary);
			source << go;
		}
		const auto log = dir / "go-build.log";
		REQUIRE(run("cd \"" + goDir.string()
							+ "\" && GOOS=wasip1 GOARCH=wasm go build ./... > \""
							+ log.string() + "\" 2>&1",
				log));
	} else {
		WARN("go not found; skipped the Go wasm build");
	}

	const std::string dart = WASM::Bindings::emit(schema, WASM::Bindings::Language::Dart);
	REQUIRE(dart.find("typedef WasmExternref = Pointer<Void>;") != std::string::npos);
	REQUIRE(dart.find("@Native<Int32 Function(Pointer<Void>)>") != std::string::npos);
}


TEST_CASE("the binding schema projects component functions", "[bindings]")
{
	WASM::Component component;
	loadTestComponent("component_runtime_import", component);
	const Json importSchema = WASM::Bindings::componentSchema(component,
			testStore().types(), WASM::Bindings::Direction::GuestToHost,
			"component_runtime_import");
	const Json exportSchema = WASM::Bindings::componentSchema(component,
			testStore().types(), WASM::Bindings::Direction::HostToGuest,
			"component_runtime_import");

	REQUIRE(importSchema.at("kind") == "component");
	const Json* import = findByName(importSchema.at("imports"), "host-add");
	REQUIRE(import != nullptr);
	REQUIRE(import->at("kind") == "function");
	REQUIRE(import->at("type").at("kind") == "func");
	REQUIRE(import->at("type").at("params").size() == 2u);
	REQUIRE(import->at("type").at("params")[0].at("type") == "u32");
	REQUIRE(import->at("type").at("params")[0].at("name") == "a");
	REQUIRE(import->at("type").at("results")[0].at("type") == "u32");

	const Json* exportEntry = findByName(exportSchema.at("exports"), "host-add");
	REQUIRE(exportEntry != nullptr);
	REQUIRE(exportEntry->at("kind") == "function");
	REQUIRE(exportEntry->at("type").at("kind") == "func");
	REQUIRE(exportEntry->at("type").at("params").size() == 2u);
	REQUIRE(exportEntry->at("type").at("results")[0].at("type") == "u32");
	REQUIRE(exportEntry->at("abi").at("params").size() == 2u);
	REQUIRE(exportEntry->at("abi").at("params")[0].at("type") == "i32");
	REQUIRE(exportEntry->at("abi").at("results")[0].at("type") == "i32");
	REQUIRE(exportEntry->at("abi").at("indirectParams") == false);
	REQUIRE(exportEntry->at("abi").at("indirectResults") == false);
}

TEST_CASE("component schema directions split imports and exports", "[bindings]")
{
	WASM::Component component;
	loadTestComponent("component_runtime_import", component);
	const auto schema = [&](WASM::Bindings::Direction direction) {
		return WASM::Bindings::componentSchema(component, testStore().types(),
				direction, "component_runtime_import");
	};

	const Json g2h = schema(WASM::Bindings::Direction::GuestToHost);
	REQUIRE(g2h.at("direction") == "g2h");
	REQUIRE_FALSE(g2h.at("imports").empty());
	REQUIRE(g2h.at("exports").empty());

	const Json h2g = schema(WASM::Bindings::Direction::HostToGuest);
	REQUIRE(h2g.at("direction") == "h2g");
	REQUIRE(h2g.at("imports").empty());
	REQUIRE_FALSE(h2g.at("exports").empty());

	const Json g2g = schema(WASM::Bindings::Direction::GuestToGuest);
	REQUIRE(g2g.at("direction") == "g2g");
	REQUIRE_FALSE(g2g.at("imports").empty());
	REQUIRE(g2g.at("exports").empty());
	const Json* g2gImport = findByName(g2g.at("imports"), "host-add");
	REQUIRE(g2gImport != nullptr);
	REQUIRE(g2gImport->at("module") == "component_runtime_import");
}

TEST_CASE("the binding schema projects component records and type imports", "[bindings]")
{
	WASM::Component component;
	loadTestComponent("component_typeimports", component);
	const Json importSchema = WASM::Bindings::componentSchema(component,
			testStore().types(), WASM::Bindings::Direction::GuestToHost,
			"component_typeimports");
	const Json exportSchema = WASM::Bindings::componentSchema(component,
			testStore().types(), WASM::Bindings::Direction::HostToGuest,
			"component_typeimports");

	const Json* import = findByName(importSchema.at("imports"), "r");
	REQUIRE(import != nullptr);
	REQUIRE(import->at("kind") == "type");

	const Json* exportEntry = findByName(exportSchema.at("exports"), "t");
	REQUIRE(exportEntry != nullptr);
	REQUIRE(exportEntry->at("kind") == "type");
	REQUIRE(exportEntry->at("type").at("kind") == "record");
	REQUIRE(exportEntry->at("type").at("fields")[0].at("name") == "x");
	REQUIRE(exportEntry->at("type").at("fields")[0].at("type").at("type") == "u32");
}


TEST_CASE("component schemas feed the language emitters", "[bindings]")
{
	WASM::Component component;
	loadTestComponent("component_runtime_import", component);
	const Json schema = WASM::Bindings::componentSchema(component, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "component_runtime_import");
	const Json importSchema = WASM::Bindings::componentSchema(component,
			testStore().types(), WASM::Bindings::Direction::GuestToHost,
			"component_runtime_import");
	const std::filesystem::path dir = bindingsDir();

	const auto write = [&](std::string_view name, const std::string& text) {
		const std::filesystem::path path = dir / std::string(name);
		std::ofstream out(path, std::ios::binary);
		out << text;
		return path;
	};
	const auto run = [&](const std::string& command,
							  const std::filesystem::path& log) {
		INFO(readText(log));
		return std::system(command.c_str()) == 0;
	};

	const std::string c = WASM::Bindings::emit(schema, WASM::Bindings::Language::CHeader);
	REQUIRE(c.find("int32_t wasmjit_host_add") != std::string::npos);
	if (haveTool("command -v gcc > /dev/null 2>&1")) {
		const auto path = write("component.h", c);
		const auto source = write("component.c",
				"#include \"component.h\"\nint main(void) { return 0; }\n");
		const auto log = dir / "gcc-component.log";
		REQUIRE(run("gcc -std=c11 -fsyntax-only -I\"" + dir.string() + "\" \""
							+ source.string() + "\" > \"" + log.string() + "\" 2>&1",
				log));
	} else {
		WARN("gcc not found; skipped the component C header check");
	}

	const std::string ts = WASM::Bindings::emit(schema, WASM::Bindings::Language::TypeScript);
	REQUIRE(ts.find("wasmjit_host_add") != std::string::npos);
	if (haveTool("command -v tsc > /dev/null 2>&1")) {
		const auto path = write("component.d.ts", ts);
		const auto log = dir / "tsc-component.log";
		REQUIRE(run("tsc --noEmit --strict \"" + path.string() + "\" > \""
							+ log.string() + "\" 2>&1",
				log));
	} else {
		WARN("tsc not found; skipped the component TypeScript check");
	}

	const std::string rust = WASM::Bindings::emit(schema, WASM::Bindings::Language::Rust);
	REQUIRE(rust.find("wasmjit_host_add") != std::string::npos);
	if (haveTool("command -v rustc > /dev/null 2>&1")) {
		const auto path = write("component.rs", rust);
		const auto lib = dir / "component.rlib";
		const auto log = dir / "rustc-component.log";
		REQUIRE(run("rustc --crate-type lib --edition 2021 \"" + path.string()
							+ "\" -o \"" + lib.string() + "\" > \"" + log.string()
							+ "\" 2>&1",
				log));
	} else {
		WARN("rustc not found; skipped the component Rust check");
	}

	const std::string zig = WASM::Bindings::emit(importSchema,
			WASM::Bindings::Language::Zig);
	REQUIRE(zig.find(".library_name = \"component\"") != std::string::npos);
	if (haveTool("command -v zig > /dev/null 2>&1")) {
		const auto path = write("component.zig", zig);
		const auto log = dir / "zig-component.log";
		REQUIRE(run("zig ast-check \"" + path.string() + "\" > \"" + log.string()
							+ "\" 2>&1",
				log));
	} else {
		WARN("zig not found; skipped the component Zig check");
	}

	const std::string go = WASM::Bindings::emit(schema, WASM::Bindings::Language::Go);
	const std::string goImport = WASM::Bindings::emit(importSchema,
			WASM::Bindings::Language::Go);
	REQUIRE(go.find("//go:wasmexport host-add") != std::string::npos);
	REQUIRE(goImport.find("//go:wasmimport component host-add") != std::string::npos);
	if (haveTool("command -v go > /dev/null 2>&1")) {
		const std::filesystem::path goDir = dir / "componentgopkg";
		std::filesystem::create_directories(goDir);
		{
			std::ofstream mod(goDir / "go.mod", std::ios::binary);
			mod << "module example.com/componentbindings\n\ngo 1.24\n";
			std::ofstream source(goDir / "bindings.go", std::ios::binary);
			source << go;
		}
		const auto log = dir / "go-component.log";
		REQUIRE(run("cd \"" + goDir.string()
							+ "\" && GOOS=wasip1 GOARCH=wasm go build ./... > \""
							+ log.string() + "\" 2>&1",
				log));
	} else {
		WARN("go not found; skipped the component Go build");
	}

	const std::string kotlin = WASM::Bindings::emit(importSchema,
			WASM::Bindings::Language::Kotlin);
	REQUIRE(kotlin.find("external fun wasmjit_component_host_add") != std::string::npos);
	const std::string moonbit = WASM::Bindings::emit(importSchema,
			WASM::Bindings::Language::MoonBit);
	REQUIRE(moonbit.find("extern \"wasm\" fn wasmjit_component_host_add")
			!= std::string::npos);
	const std::string dart = WASM::Bindings::emit(importSchema,
			WASM::Bindings::Language::Dart);
	REQUIRE(dart.find("@Native<Int32 Function(Int32, Int32)>") != std::string::npos);

	const std::string cpp = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CppTypedefs);
	REQUIRE(cpp.find("using wasmjit_host_add_fn = int32_t (*)(WASM::VMContext* "
			"context, int32_t arg0, int32_t arg1);") != std::string::npos);
	const std::string cppSource = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CppSource);
	REQUIRE(cppSource.find("wasmjit_host_add_fn wasmjit_host_add = nullptr;")
			!= std::string::npos);
}


TEST_CASE("non-function entries stay visible in emitted bindings", "[bindings]")
{
	WASM::Component component;
	loadTestComponent("component_typeimports", component);
	const Json importSchema = WASM::Bindings::componentSchema(component,
			testStore().types(), WASM::Bindings::Direction::GuestToHost,
			"component_typeimports");
	const Json exportSchema = WASM::Bindings::componentSchema(component,
			testStore().types(), WASM::Bindings::Direction::HostToGuest,
			"component_typeimports");
	const std::string c = WASM::Bindings::emit(importSchema,
			WASM::Bindings::Language::CHeader);
	REQUIRE(c.find("Non-function entries") != std::string::npos);
	REQUIRE(c.find("// import: type \"r\"") != std::string::npos);
	REQUIRE(WASM::Bindings::emit(exportSchema, WASM::Bindings::Language::CHeader)
			.find("// export: type \"t\" (record)") != std::string::npos);

	WASM::Component values;
	loadTestComponent("component_instance_args", values);
	const Json valueSchema = WASM::Bindings::componentSchema(values,
			testStore().types(), WASM::Bindings::Direction::GuestToHost,
			"component_instance_args");
	const std::string ts = WASM::Bindings::emit(valueSchema,
			WASM::Bindings::Language::TypeScript);
	REQUIRE(ts.find("// import: value \"hostv\" (u32)") != std::string::npos);

	const Json core = WASM::Bindings::moduleSchema(loadTestModule("asmjit_i32_basic"),
			testStore().types(), WASM::Bindings::Direction::GuestToGuest,
			"asmjit_i32_basic");
	const std::string header = WASM::Bindings::emit(core,
			WASM::Bindings::Language::CHeader);
	REQUIRE(header.find("// import: global \"g\" (i32)") != std::string::npos);
}

TEST_CASE("component canonical ABI flattening feeds the emitters", "[bindings]")
{
	WASM::Component component;
	loadTestComponent("component_runtime_string", component);
	const Json schema = WASM::Bindings::componentSchema(component, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "component_runtime_string");

	const Json* echo = findByName(schema.at("exports"), "echo");
	REQUIRE(echo != nullptr);
	REQUIRE(echo->at("type").at("params")[0].at("type") == "string");
	REQUIRE(echo->at("abi").at("params").size() == 2u);
	REQUIRE(echo->at("abi").at("params")[0].at("type") == "i32");
	REQUIRE(echo->at("abi").at("results").size() == 1u);
	REQUIRE(echo->at("abi").at("results")[0].at("type") == "i32");
	REQUIRE(echo->at("abi").at("indirectResults") == true);

	const std::string c = WASM::Bindings::emit(schema, WASM::Bindings::Language::CHeader);
	REQUIRE(c.find("int32_t wasmjit_echo(int32_t arg0, int32_t arg1)")
			!= std::string::npos);
	const std::string rust = WASM::Bindings::emit(schema, WASM::Bindings::Language::Rust);
	REQUIRE(rust.find("fn(arg0: i32, arg1: i32) -> i32") != std::string::npos);
	const std::string ts = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::TypeScript);
	REQUIRE(ts.find("wasmjit_echo(arg0: number, arg1: number): number")
			!= std::string::npos);
	const std::string go = WASM::Bindings::emit(schema, WASM::Bindings::Language::Go);
	REQUIRE(go.find("func wasmjit_echo(arg0 int32, arg1 int32) int32")
			!= std::string::npos);
}


TEST_CASE("stringref uses the opaque reference spelling in every emitter", "[bindings]")
{
	const std::vector<uint8_t> bytes = stringRefModuleBytes();
	const std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	const std::unique_ptr<WASM::Artifact> artifact =
			WASM::Artifact::parse(device, testStore());
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isCoreModule());
	const WASM::Module& module = *artifact->asCoreModule();

	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "stringref_echo");
	const Json* echo = findExport(schema, "echo");
	REQUIRE(echo != nullptr);
	REQUIRE(echo->at("type").at("params")[0].at("type") == "stringref");
	REQUIRE(echo->at("type").at("results")[0].at("type") == "stringref");

	const std::string c = WASM::Bindings::emit(schema, WASM::Bindings::Language::CHeader);
	REQUIRE(c.find("typedef struct wasm_stringref wasm_stringref;") != std::string::npos);
	REQUIRE(c.find("wasm_stringref* arg0") != std::string::npos);

	const std::string rust = WASM::Bindings::emit(schema, WASM::Bindings::Language::Rust);
	REQUIRE(rust.find("pub struct wasm_stringref") != std::string::npos);
	REQUIRE(rust.find("*mut wasm_stringref") != std::string::npos);

	const std::string ts = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::TypeScript);
	REQUIRE(ts.find("interface WasmStringref") != std::string::npos);
	REQUIRE(ts.find("arg0: WasmStringref") != std::string::npos);

	const std::string as = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::AssemblyScript);
	REQUIRE(as.find("externref") != std::string::npos);

	const std::string zig = WASM::Bindings::emit(schema, WASM::Bindings::Language::Zig);
	REQUIRE(zig.find("pub const wasm_stringref = opaque {};") != std::string::npos);
	REQUIRE(zig.find("*wasm_stringref") != std::string::npos);

	const std::string kotlin = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::Kotlin);
	REQUIRE(kotlin.find("class WasmStringref private constructor()")
			!= std::string::npos);
	REQUIRE(kotlin.find("typealias wasmjit_echo_t = (WasmStringref) -> WasmStringref")
			!= std::string::npos);

	const std::string moonbit = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::MoonBit);
	REQUIRE(moonbit.find("#external\ntype WasmStringref") != std::string::npos);
	REQUIRE(moonbit.find("// H2G export: echo(WasmStringref) -> WasmStringref")
			!= std::string::npos);

	const std::string go = WASM::Bindings::emit(schema, WASM::Bindings::Language::Go);
	REQUIRE(go.find("type WasmStringref struct") != std::string::npos);
	REQUIRE(go.find("arg0 uintptr") != std::string::npos);

	const std::string dart = WASM::Bindings::emit(schema, WASM::Bindings::Language::Dart);
	REQUIRE(dart.find("typedef WasmStringref = Pointer<Void>;") != std::string::npos);
	REQUIRE(dart.find("Pointer<Void> arg0") != std::string::npos);
}


TEST_CASE("a generated C++ adapter calls an export and reports a trap", "[bindings]")
{
	runCppAdapterConformance("asmjit_i32_basic", R"cpp(
#include "adapter.h"
#include <WasmBase/WasmRuntime.hpp>
#include <WasmBase/WasmTrap.hpp>
#include <WasmBase/WasmRegistryImportResolver.hpp>
extern WASM::ModuleInstance* wasmjit_adapter_instance;

int main(int argc, char** argv)
{
    auto runtime = WASM::Runtime::create("libjit");
    auto module = runtime->loadFile(argv[1]);
    WASM::RegistryImportResolver imports;
    auto& instance = runtime->instantiate(*module, imports);
    wasmjit_adapter_instance = &instance;
    if (wasmjit_add(2, 3) != 5)
        return 1;
    try {
        (void)wasmjit_div_s(1, 0);
        return 2;
    } catch (const WASM::Trap&) {
    }
    return 0;
}
)cpp", "adapter-call");
}

TEST_CASE("a generated C++ adapter satisfies an import and passes a reference", "[bindings]")
{
	runCppAdapterConformance("wasm_js_string_length", R"cpp(
#include "adapter.h"
#include <WasmBase/WasmRuntime.hpp>
#include <WasmBase/WasmRegistryImportResolver.hpp>
#include <cstdint>

extern WASM::ModuleInstance* wasmjit_adapter_instance;

namespace {
int32_t hostLength(void*) { return 7; }
}

int main(int argc, char** argv)
{
    auto runtime = WASM::Runtime::create("libjit");
    auto module = runtime->loadFile(argv[1]);
    WASM::RegistryImportResolver imports;
    imports.registerFunction("wasm:js-string", "length",
        WASM::Callable{reinterpret_cast<void*>(hostLength), nullptr, 0});
    auto& instance = runtime->instantiate(*module, imports);
    wasmjit_adapter_instance = &instance;
    int token = 0;
    auto* ref = reinterpret_cast<wasm_externref*>(&token);
    if (wasmjit_len(ref) != 7)
        return 1;
    if (wasmjit_identity(ref) != ref)
        return 2;
    return 0;
}
)cpp", "adapter-ref");
}

TEST_CASE("a generated C++ adapter calls a multi-value export", "[bindings]")
{
	runCppAdapterConformance("multi_value", R"cpp(
#include "adapter.h"
#include <WasmBase/WasmRuntime.hpp>
#include <WasmBase/WasmRegistryImportResolver.hpp>
#include <cstdint>

extern WASM::ModuleInstance* wasmjit_adapter_instance;

namespace {
struct Pair {
    int32_t first;
    int32_t second;
};
}

int main(int argc, char** argv)
{
    auto runtime = WASM::Runtime::create("libjit");
    auto module = runtime->loadFile(argv[1]);
    WASM::RegistryImportResolver imports;
    auto& instance = runtime->instantiate(*module, imports);
    wasmjit_adapter_instance = &instance;
    Pair pair{};
    wasmjit_swap_export(3, 4, &pair);
    if (pair.first != 4 || pair.second != 3)
        return 1;
    return 0;
}
)cpp", "adapter-multi");
}

TEST_CASE("a generated C++ adapter owns and releases a reference result", "[bindings]")
{
	runCppAdapterConformance("bindings_ref_ownership", R"cpp(
#include "adapter.h"
#include <WasmBase/WasmRuntime.hpp>
#include <WasmBase/WasmRegistryImportResolver.hpp>
extern WASM::ModuleInstance* wasmjit_adapter_instance;

int main(int argc, char** argv)
{
    auto runtime = WASM::Runtime::create("libjit");
    auto module = runtime->loadFile(argv[1]);
    WASM::RegistryImportResolver imports;
    auto& instance = runtime->instantiate(*module, imports);
    wasmjit_adapter_instance = &instance;
    wasm_structref* node = wasmjit_make();
    if (node == nullptr)
        return 1;
    if (wasmjit_get(node) != 42)
        return 2;
    wasm_structref* retained =
        static_cast<wasm_structref*>(instance.refClone(node));
    instance.refDrop(retained);
    instance.refDrop(node);
    return 0;
}
)cpp", "adapter-ownership");
}

TEST_CASE("SWIG candidates are validated against the engine schema", "[bindings]")
{
	if (!haveTool("command -v swig > /dev/null 2>&1")) {
		WARN("swig not found; skipped the C++ candidate front-end check");
		return;
	}

	const std::filesystem::path dir = bindingsDir() / "frontend-cpp";
	std::filesystem::create_directories(dir);
	const std::filesystem::path header = dir / "candidates.h";
	{
		std::ofstream out(header, std::ios::binary);
		out << "#include <cstdint>\n"
			   "int32_t add(int32_t a, int32_t b);\n"
			   "int32_t div_s(int32_t a, int32_t b);\n"
			   "int32_t sub(int32_t a);\n";
	}

	const WASM::Module module = loadTestModule("asmjit_i32_basic");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "asmjit_i32_basic");

	std::string error;
	const auto candidates = WASM::Bindings::swigCandidates(header, error);
	REQUIRE(error.empty());
	REQUIRE(candidates.size() == 3u);

	const Json report = WASM::Bindings::crossCheckCandidates(schema, candidates);
	REQUIRE(report.at("matched") == 2);
	REQUIRE(report.at("unmatched") == 1);
	const Json* add = nullptr;
	const Json* divide = nullptr;
	const Json* mismatch = nullptr;
	for (const Json& entry : report.at("candidates")) {
		if (entry.value("name", "") == "add")
			add = &entry;
		else if (entry.value("name", "") == "div_s")
			divide = &entry;
		else if (entry.value("name", "") == "sub")
			mismatch = &entry;
	}
	REQUIRE(add != nullptr);
	REQUIRE(divide != nullptr);
	REQUIRE(mismatch != nullptr);
	REQUIRE(add->at("matched") == true);
	REQUIRE(divide->at("matched") == true);
	REQUIRE(mismatch->at("matched") == false);
	REQUIRE(mismatch->at("reason").get<std::string>().find("parameter count")
			!= std::string::npos);
}


TEST_CASE("Glue candidates are validated against the engine schema", "[bindings]")
{
	if (!haveTool("command -v g++ > /dev/null 2>&1")
			|| !std::filesystem::is_regular_file(
					"/usr/local/lib/Glue-1.5/libGlue.a")) {
		WARN("Glue or g++ not found; skipped the Glue front-end check");
		return;
	}

	const std::filesystem::path dir = bindingsDir() / "frontend-cpp";
	std::filesystem::create_directories(dir);
	const std::filesystem::path source = dir / "glue_bindings.cpp";
	{
		std::ofstream out(source, std::ios::binary);
		out << "#include <glue/value.h>\n"
			   "#include <cstdint>\n"
			   "namespace {\n"
			   "int32_t add(int32_t a, int32_t b) { return a + b; }\n"
			   "int32_t div_s(int32_t a, int32_t b) { return a / b; }\n"
			   "int32_t sub(int32_t a) { return a; }\n"
			   "}\n"
			   "glue::MapValue wasmjit_bindings()\n"
			   "{\n"
			   "\tauto map = glue::createAnyMap();\n"
			   "\tmap[\"add\"] = glue::AnyFunction(add);\n"
			   "\tmap[\"div_s\"] = glue::AnyFunction(div_s);\n"
			   "\tmap[\"sub\"] = glue::AnyFunction(sub);\n"
			   "\treturn map;\n"
			   "}\n";
	}

	const WASM::Module module = loadTestModule("asmjit_i32_basic");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "asmjit_i32_basic");

	std::string error;
	const auto candidates = WASM::Bindings::glueCandidates(source, error);
	REQUIRE(error.empty());
	REQUIRE(candidates.size() == 3u);

	const Json report = WASM::Bindings::crossCheckCandidates(schema, candidates,
			"glue");
	REQUIRE(report.at("frontend") == "glue");
	REQUIRE(report.at("matched") == 2);
	REQUIRE(report.at("unmatched") == 1);
	const Json* mismatch = nullptr;
	for (const Json& entry : report.at("candidates")) {
		if (entry.value("name", "") == "sub")
			mismatch = &entry;
	}
	REQUIRE(mismatch != nullptr);
	REQUIRE(mismatch->at("matched") == false);
	REQUIRE(mismatch->at("reason").get<std::string>().find("parameter count")
			!= std::string::npos);
}

TEST_CASE("C++ host functions with VMContext* produce a G2H schema", "[bindings]")
{
	if (!haveTool("command -v swig > /dev/null 2>&1")) {
		WARN("swig not found; skipped the front-end schema check");
		return;
	}

	const std::filesystem::path dir = bindingsDir() / "frontend-cpp";
	std::filesystem::create_directories(dir);
	const std::filesystem::path header = dir / "host_functions.h";
	{
		std::ofstream out(header, std::ios::binary);
		out << "#include <cstdint>\n"
			   "namespace WASM { struct VMContext; }\n"
			   "/*! Adds two numbers. */\n"
			   "int32_t host_add(WASM::VMContext*, int32_t a, int32_t b);\n"
			   "int32_t not_exposed(int32_t a);\n";
	}

	std::string error;
	const auto candidates = WASM::Bindings::swigCandidates(header, error);
	REQUIRE(error.empty());
	const Json schema = WASM::Bindings::candidateSchema(candidates, "g2h",
			"host_functions", "swig");
	REQUIRE(schema.at("direction") == "g2h");
	REQUIRE(schema.at("frontend") == "swig");
	REQUIRE(schema.at("imports").size() == 1u);
	REQUIRE(schema.at("exports").empty());
	const Json& importEntry = schema.at("imports")[0];
	REQUIRE(importEntry.at("name") == "host_add");
	REQUIRE(importEntry.at("module") == "host_functions");
	REQUIRE(importEntry.at("description") == "Adds two numbers.");
	REQUIRE(importEntry.at("argNames").size() == 2u);
	REQUIRE(importEntry.at("argNames")[0] == "a");
	REQUIRE(importEntry.at("argNames")[1] == "b");
	REQUIRE(importEntry.at("type").at("params").size() == 2u);
	REQUIRE(importEntry.at("type").at("params")[0].at("type") == "i32");
	REQUIRE(importEntry.at("type").at("results")[0].at("type") == "i32");

	const auto typed = WASM::Bindings::candidateSchemaModel(candidates,
			WASM::Bindings::Direction::GuestToHost, "host_functions", "swig");
	const std::string c = WASM::Bindings::emit(typed,
			WASM::Bindings::Language::CHeader);
	REQUIRE(c.find("WASMJIT_WASM_IMPORT(\"host_functions\", \"host_add\")")
			!= std::string::npos);
	REQUIRE(c.find("int32_t a, int32_t b") != std::string::npos);
	const std::string rust = WASM::Bindings::emit(typed,
			WASM::Bindings::Language::Rust);
	REQUIRE(rust.find("#[link(wasm_import_module = \"host_functions\")]")
			!= std::string::npos);
	REQUIRE(rust.find("pub fn wasmjit_host_functions_host_add(a: i32, b: i32)")
			!= std::string::npos);
	const std::string ts = WASM::Bindings::emit(typed,
			WASM::Bindings::Language::TypeScript);
	REQUIRE(ts.find("wasmjit_host_functions_host_add(a: number, b: number): number")
			!= std::string::npos);
	REQUIRE(ts.find("Adds two numbers.") != std::string::npos);
	const std::string java = WASM::Bindings::emit(typed,
			WASM::Bindings::Language::Java);
	REQUIRE(java.find("wasmjit_host_functions_host_add(int a, int b)")
			!= std::string::npos);

	if (std::filesystem::is_regular_file(
				"/usr/local/lib/Glue-1.5/libGlue.a")) {
		const std::filesystem::path glueSource = dir / "host_glue.cpp";
		{
			std::ofstream out(glueSource, std::ios::binary);
			out << "#include <glue/value.h>\n"
				   "#include <cstdint>\n"
				   "namespace WASM { struct VMContext; }\n"
				   "/*! Adds two numbers. */\n"
				   "int32_t host_add(WASM::VMContext*, int32_t a, int32_t b) "
				   "{ return a + b; }\n"
				   "int32_t not_exposed(int32_t a) { return a; }\n"
				   "glue::MapValue wasmjit_bindings()\n"
				   "{\n"
				   "\tauto map = glue::createAnyMap();\n"
				   "\tmap[\"host_add\"] = glue::AnyFunction(host_add);\n"
				   "\tmap[\"not_exposed\"] = glue::AnyFunction(not_exposed);\n"
				   "\treturn map;\n"
				   "}\n";
		}
		std::string glueError;
		const auto glueCandidates =
				WASM::Bindings::glueCandidates(glueSource, glueError);
		REQUIRE(glueError.empty());
		const Json glueSchema = WASM::Bindings::candidateSchema(glueCandidates,
				"g2h", "host_glue", "glue");
		REQUIRE(glueSchema.at("imports").size() == 1u);
		REQUIRE(glueSchema.at("imports")[0].at("name") == "host_add");
		REQUIRE(glueSchema.at("imports")[0].at("description") == "Adds two numbers.");
		REQUIRE(glueSchema.at("imports")[0].at("argNames")[0] == "a");
		REQUIRE(glueSchema.at("imports")[0].at("argNames")[1] == "b");
	}
}

TEST_CASE("host-side callable typedefs follow the engine ABI", "[bindings]")
{
	const Json schema = WASM::Bindings::moduleSchema(
			loadTestModule("asmjit_i32_basic"), testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "asmjit_i32_basic");
	const std::string header = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CppTypedefs);
	REQUIRE(header.find("using wasmjit_add_fn = int32_t (*)(WASM::VMContext* "
			"context, int32_t arg0, int32_t arg1);") != std::string::npos);
	REQUIRE(header.find("extern wasmjit_add_fn wasmjit_add;")
			!= std::string::npos);
	REQUIRE(header.find("namespace WASM { struct VMContext; struct ModuleInstance; class RegistryImportResolver; }")
			!= std::string::npos);
	REQUIRE(header.find("#include <cstdint>") != std::string::npos);
	// A v128 is two scalar words, low first: a single 16-byte aggregate would be
	// passed by reference on MS x64 (docs/ABI.md “v128 across the Boundary”).
	REQUIRE(header.find("struct wasm_v128 { std::uint64_t lo; std::uint64_t hi; };")
			!= std::string::npos);
	REQUIRE(header.find("#ifdef __cplusplus") == std::string::npos);
	REQUIRE(header.find("extern \"C\" {") == std::string::npos);
	const std::size_t loaderDeclaration = header.find(
			"bool wasmjit_asmjit_i32_basic_load_callables(");
	REQUIRE(loaderDeclaration != std::string::npos);

	const std::string source = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CppSource);
	REQUIRE(source.find("extern \"C\"") == std::string::npos);
	REQUIRE(source.find("wasmjit_add_fn wasmjit_add = nullptr;")
			!= std::string::npos);
	REQUIRE(source.find("wasmjit_asmjit_i32_basic_load_callables("
			"WASM::ModuleInstance* instance)") != std::string::npos);
	REQUIRE(source.find("std::array<std::string_view,") != std::string::npos);
	REQUIRE(source.find("instance->exportedFunctions(names, callables)")
			!= std::string::npos);
	REQUIRE(source.find("using Target = std::decay_t<decltype(target)>;")
			!= std::string::npos);
	REQUIRE(source.find("target = reinterpret_cast<Target>(callable->fnPtr);")
			!= std::string::npos);
	REQUIRE(source.find("assignCallable(callables[0], wasmjit_add);")
			!= std::string::npos);
	const std::size_t firstIf = source.find("if (!callable)");
	REQUIRE(firstIf != std::string::npos);
	REQUIRE(firstIf == source.rfind("if (!callable)"));

	const Json g2h = WASM::Bindings::moduleSchema(
			loadTestModule("wasm_js_string_length"), testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "wasm_js_string_length");
	const std::string importHeader = WASM::Bindings::emit(g2h,
			WASM::Bindings::Language::CppTypedefs);
	REQUIRE(importHeader.find("int32_t wasmjit_wasm_js_string_length(")
			!= std::string::npos);
	REQUIRE(importHeader.find("using wasmjit_wasm_js_string_length_fn")
			== std::string::npos);
	const std::size_t importLinkage = importHeader.find("extern \"C\" {");
	const std::size_t importDeclaration = importHeader.find(
			"int32_t wasmjit_wasm_js_string_length(");
	const std::size_t importLinkageEnd = importHeader.find("} // extern \"C\"");
	const std::size_t registrationDeclaration = importHeader.find(
			"bool wasmjit_wasm_js_string_length_register_imports("
			"WASM::RegistryImportResolver& registrar);");
	REQUIRE(importHeader.find("#ifdef __cplusplus") == std::string::npos);
	REQUIRE(importHeader.find("#include <cstdint>") != std::string::npos);
	REQUIRE(importLinkage != std::string::npos);
	REQUIRE(importDeclaration > importLinkage);
	REQUIRE(importLinkageEnd > importDeclaration);
	REQUIRE(registrationDeclaration > importLinkageEnd);
	const std::string importSource = WASM::Bindings::emit(g2h,
			WASM::Bindings::Language::CppSource);
	REQUIRE(importSource.find("extern \"C\"") == std::string::npos);
	REQUIRE(importSource.find("int32_t wasmjit_wasm_js_string_length(")
			!= std::string::npos);
	REQUIRE(importSource.find("TODO: implement this host import.")
			!= std::string::npos);
	REQUIRE(importSource.find("#include <WasmBase/WasmRegistryImportResolver.hpp>")
			!= std::string::npos);
	REQUIRE(importSource.find("#include <WasmBase/WasmNativeImport.hpp>")
			!= std::string::npos);
	REQUIRE(importSource.find("bool wasmjit_wasm_js_string_length_register_imports("
			"WASM::RegistryImportResolver& registrar)") != std::string::npos);
	REQUIRE(importSource.find("registrar.moduleRegistry(\"wasm:js-string\")")
			!= std::string::npos);
	REQUIRE(importSource.find("module.registerFunctions({") != std::string::npos);
	REQUIRE(importSource.find("WASM::nativeImport<&wasmjit_wasm_js_string_length>()")
			!= std::string::npos);

	if (haveTool("command -v g++ > /dev/null 2>&1")) {
		const std::filesystem::path dir = bindingsDir() / "cpp-callables";
		std::filesystem::create_directories(dir);
		{
			std::ofstream out(dir / "wasmjit_asmjit_i32_basic_callables.hpp",
					std::ios::binary);
			out << header;
			std::ofstream sourceOut(dir / "callables.cpp", std::ios::binary);
			sourceOut << source;
		}
		const std::filesystem::path sourceRoot =
				std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR).parent_path()
						.parent_path();
		{
			std::ofstream out(dir / "wasmjit_wasm_js_string_length_callables.hpp",
					std::ios::binary);
			out << importHeader;
			std::ofstream stubOut(dir / "import_stubs.cpp", std::ios::binary);
			stubOut << importSource;
		}
		const std::filesystem::path importLog = dir / "g++-import-stubs.log";
		const std::string compileImports = "g++ -std=c++2a -fsyntax-only -I\"" + dir.string()
				+ "\" -I\"" + sourceRoot.string() + "\" -I\""
				+ (sourceRoot / "extern" / "MhLib").string() + "\" \""
				+ (dir / "import_stubs.cpp").string() + "\" > \"" + importLog.string()
				+ std::string(1, '"') + " 2>&1";

		INFO(readText(importLog));
		REQUIRE(std::system(compileImports.c_str()) == 0);

		const std::filesystem::path log = dir / "g++-callables.log";
		const std::string compile = "g++ -std=c++2a -fsyntax-only -I\"" + dir.string()
				+ "\" -I\"" + sourceRoot.string() + "\" -I\""
				+ (sourceRoot / "extern" / "MhLib").string() + "\" \""
				+ (dir / "callables.cpp").string() + "\" > \"" + log.string()
				+ "\" 2>&1";
		INFO(readText(log));
		REQUIRE(std::system(compile.c_str()) == 0);
	}

	const Json multi = WASM::Bindings::moduleSchema(loadTestModule("multi_value"),
			testStore().types(), WASM::Bindings::Direction::HostToGuest,
			"multi_value");
	const std::string multiHeader = WASM::Bindings::emit(multi,
			WASM::Bindings::Language::CppTypedefs);
	REQUIRE(multiHeader.find("using wasmjit_swap_export_fn = void (*)(WASM::VMContext* "
			"context, int32_t arg0, int32_t arg1, void* result);")
			!= std::string::npos);
}

TEST_CASE("generated h2g callable loader resolves guest exports", "[bindings]")
{
	if (!haveTool("command -v g++ > /dev/null 2>&1")) {
		WARN("g++ not found; skipped the generated callable-loader check");
		return;
	}

	const std::filesystem::path dir = bindingsDir() / "cpp-loader";
	std::filesystem::create_directories(dir);
	const Json schema = WASM::Bindings::moduleSchema(
			loadTestModule("asmjit_i32_basic"), testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "asmjit_i32_basic");
	const auto write = [&](std::string_view name, const std::string& text) {
		std::ofstream out(dir / std::string(name), std::ios::binary);
		out << text;
	};
	write("wasmjit_asmjit_i32_basic_callables.hpp",
			WASM::Bindings::emit(schema, WASM::Bindings::Language::CppTypedefs));
	write("callables.cpp",
			WASM::Bindings::emit(schema, WASM::Bindings::Language::CppSource));
	write("main.cpp", R"cpp(
#include "wasmjit_asmjit_i32_basic_callables.hpp"
#include <WasmBase/WasmRuntime.hpp>
#include <WasmBase/WasmModuleInstance.hpp>
#include <WasmBase/WasmRegistryImportResolver.hpp>

int main(int argc, char** argv)
{
    auto runtime = WASM::Runtime::create("libjit");
    auto module = runtime->loadFile(argv[1]);
    WASM::RegistryImportResolver imports;
    auto& instance = runtime->instantiate(*module, imports);
    if (!wasmjit_asmjit_i32_basic_load_callables(&instance))
        return 1;
    if (wasmjit_add(instance.context(), 2, 3) != 5)
        return 2;
    return 0;
}
)cpp");

	const std::filesystem::path sourceRoot =
			std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR).parent_path()
					.parent_path();
	const std::filesystem::path buildRoot = std::filesystem::path(WASM_TEST_DIR)
			.parent_path().parent_path().parent_path();
	const std::filesystem::path log = dir / "compile.log";
	const std::string compile = "g++ -std=c++2a" + childSanitizerFlags()
			+ " -I\"" + dir.string() + "\" -I\""
			+ sourceRoot.string() + "\" -I\""
			+ (sourceRoot / "extern" / "MhLib").string() + "\" \""
			+ (dir / "main.cpp").string() + "\" \"" + (dir / "callables.cpp").string()
			+ "\" -L\"" + (buildRoot / "WasmBase").string() + "\" -L\""
			+ (buildRoot / "LibJit").string() + "\" -L\""
			+ (buildRoot / "extern" / "MhLib" / "Elvavena").string() + "\" -L\""
			+ (buildRoot / "extern" / "MhLib" / "Euphemy").string()
			+ "\" -lWasmBase -lLibJit -lEuphemy -lElvavena -ljit -Wl,-rpath,\""
			+ (buildRoot / "WasmBase").string() + ":" + (buildRoot / "LibJit").string()
			+ ":" + (buildRoot / "extern" / "MhLib" / "Elvavena").string() + ":"
			+ (buildRoot / "extern" / "MhLib" / "Euphemy").string() + "\" -o \""
			+ (dir / "loader").string() + "\" > \"" + log.string() + "\" 2>&1";
	INFO(readText(log));
	REQUIRE(std::system(compile.c_str()) == 0);

	const std::filesystem::path runLog = dir / "run.log";
	const std::string run = "\"" + (dir / "loader").string() + "\" \""
			+ (std::filesystem::path(WASM_TEST_DIR) / "asmjit_i32_basic.wasm").string()
			+ "\" > \"" + runLog.string() + "\" 2>&1";
	INFO(readText(runLog));
	REQUIRE(std::system(run.c_str()) == 0);
}

TEST_CASE("additional documented language emitters produce declarations", "[bindings]")
{
	const Json hi = WASM::Bindings::moduleSchema(loadTestModule("asmjit_i32_basic"),
			testStore().types(), WASM::Bindings::Direction::HostToGuest,
			"asmjit_i32_basic");
	const std::vector<std::pair<WASM::Bindings::Language, std::string>> targets = {
		{ WASM::Bindings::Language::Java, "teavm" },
		{ WASM::Bindings::Language::CSharp, "UnmanagedCallersOnly" },
		{ WASM::Bindings::Language::FSharp, "UnmanagedCallersOnly" },
		{ WASM::Bindings::Language::Python, "ctypes" },
		{ WASM::Bindings::Language::Ruby, "Fiddle" },
		{ WASM::Bindings::Language::Lua, "ffi.cdef" },
		{ WASM::Bindings::Language::Swift, "@convention(c)" },
		{ WASM::Bindings::Language::DLang, "extern(C)" },
		{ WASM::Bindings::Language::Nim, "cdecl" },
		{ WASM::Bindings::Language::Crystal, "lib WasmJitBindings" },
		{ WASM::Bindings::Language::Ada, "Convention => C" },
		{ WASM::Bindings::Language::Pascal, "cdecl" },
		{ WASM::Bindings::Language::Haskell, "type Wasmjit_" }
	};
	for (const auto& [language, marker] : targets) {
		INFO(WASM::Bindings::languageName(language));
		const std::string text = WASM::Bindings::emit(hi, language);
		REQUIRE_FALSE(text.empty());
		REQUIRE(text.find(marker) != std::string::npos);
		REQUIRE(text.find("add") != std::string::npos);
	}

	const Json g2h = WASM::Bindings::moduleSchema(
			loadTestModule("wasm_js_string_length"), testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "wasm_js_string_length");
	REQUIRE(WASM::Bindings::emit(g2h, WASM::Bindings::Language::Java)
			.find("wasmjit_wasm_js_string_length") != std::string::npos);
	REQUIRE(WASM::Bindings::emit(g2h, WASM::Bindings::Language::CSharp)
			.find("wasmjit_wasm_js_string_length") != std::string::npos);
	REQUIRE(WASM::Bindings::emit(g2h, WASM::Bindings::Language::Python)
			.find("wasmjit_wasm_js_string_length") != std::string::npos);
}

TEST_CASE("WIT JSON and WIT text are binding-schema inputs", "[bindings]")
{
	const Json wit = {
		{ "worlds", Json::array({
			{ { "name", "demo" },
				{ "imports", { { "host-add", { { "function", {
					{ "name", "host-add" }, { "kind", "freestanding" },
					{ "params", Json::array({ { { "name", "a" },
						{ "type", "u32" } }, { { "name", "b" },
						{ "type", "u32" } } }) },
					{ "result", "u32" } } } } } } },
				{ "exports", { { "run", { { "function", {
					{ "name", "run" }, { "kind", "freestanding" },
					{ "params", Json::array({ { { "name", "x" },
						{ "type", "u32" } } }) },
					{ "result", "u32" } } } } } } },
				{ "package", 0 } } }) },
		{ "interfaces", Json::array() },
		{ "types", Json::array() },
		{ "packages", Json::array({ { { "name", "test:demo@0.1.0" },
			{ "interfaces", Json::object() },
			{ "worlds", { { "demo", 0 } } } } }) },
	};

	std::string error;
	const auto complete = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", error);
	REQUIRE(error.empty());
	REQUIRE(complete.imports.size() == 1u);
	REQUIRE(complete.exports.size() == 1u);
	REQUIRE(complete.imports[0].name == "host-add");
	REQUIRE(complete.imports[0].argNames[1] == "b");
	const auto g2h = WASM::Bindings::projectSchema(complete,
			WASM::Bindings::Direction::GuestToHost);
	const auto h2g = WASM::Bindings::projectSchema(complete,
			WASM::Bindings::Direction::HostToGuest);
	REQUIRE(g2h.exports.empty());
	REQUIRE(h2g.imports.empty());
	REQUIRE(WASM::Bindings::emit(g2h, WASM::Bindings::Language::CHeader)
			.find("wasmjit_demo_host_add") != std::string::npos);
	REQUIRE(WASM::Bindings::emit(h2g, WASM::Bindings::Language::Rust)
			.find("wasmjit_run") != std::string::npos);

	if (haveTool("command -v wasm-tools > /dev/null 2>&1")) {
		const std::filesystem::path path = bindingsDir() / "wit-input.wit";
		{
			std::ofstream out(path, std::ios::binary);
			out << "package test:demo@0.1.0;\n"
				   "world demo {\n"
				   "  import host-add: func(a: u32, b: u32) -> u32;\n"
				   "  export run: func(x: u32) -> u32;\n"
				   "}\n";
		}
		const auto fromText = WASM::Bindings::witSchemaFromFile(path,
				WASM::Bindings::Direction::Complete, "demo", "demo", false,
				error);
		REQUIRE(error.empty());
		REQUIRE(fromText.imports.size() == 1u);
		REQUIRE(fromText.exports.size() == 1u);
		REQUIRE(WASM::Bindings::emit(fromText,
			WASM::Bindings::Language::CHeader).find("host_add")
			!= std::string::npos);
	}
}

TEST_CASE("WIT type aliases resolve through the type table", "[bindings]")
{
	const Json wit = {
		{ "worlds", Json::array({
			{ { "name", "demo" },
				{ "imports", { { "now", { { "function", {
					{ "name", "now" }, { "kind", "freestanding" },
					{ "params", Json::array({ { { "name", "duration" },
						{ "type", 1 } } }) },
					{ "result", 0 } } } } } } },
				{ "exports", Json::object() }, { "package", 0 } } }) },
		{ "interfaces", Json::array() },
		{ "types", Json::array({
			{ { "name", "mark" }, { "kind", { { "type", "u64" } } } },
			{ { "name", "duration" }, { "kind", { { "type", 0 } } } } }) },
		{ "packages", Json::array({ { { "name", "test:demo@0.1.0" },
			{ "interfaces", Json::object() },
			{ "worlds", { { "demo", 0 } } } } }) },
	};
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", error);
	REQUIRE(error.empty());
	REQUIRE(schema.imports.size() == 1u);
	REQUIRE(schema.imports[0].function.params.size() == 1u);
	REQUIRE(schema.imports[0].function.params[0].kind
			== WASM::Bindings::ValueKind::U64);
	REQUIRE(schema.imports[0].function.results.size() == 1u);
	REQUIRE(schema.imports[0].function.results[0].kind
			== WASM::Bindings::ValueKind::U64);
	REQUIRE(schema.imports[0].raw.at("abi").at("params")[0].at("type")
			== "i64");
	REQUIRE(schema.imports[0].raw.at("abi").at("results")[0].at("type")
			== "i64");
}

TEST_CASE("WIT records get named C++ declarations", "[bindings]")
{
	const Json wit = Json::parse(R"({
		"worlds": [{
			"name": "demo",
			"imports": { "echo": { "function": {
				"name": "echo",
				"params": [{ "name": "value", "type": 0 }],
				"result": 0 } } },
			"exports": {},
			"package": 0
		}],
		"interfaces": [],
		"types": [
			{ "name": "point", "kind": { "record": {
				"fields": [{ "name": "x", "type": "s32" }] } } },
			{ "name": "color", "kind": { "enum": {
				"cases": [{ "name": "red" }, { "name": "green" }] } } },
			{ "name": "perms", "kind": { "flags": {
				"flags": [{ "name": "read" }, { "name": "write" }] } } },
			{ "kind": { "list": "u8" } },
			{ "kind": { "variant": { "cases": [
				{ "name": "point", "type": 0 },
				{ "name": "empty", "type": null }] } } },
			{ "name": "descriptor", "kind": "resource" },
			{ "kind": { "handle": { "own": 5 } } }
		],
		"packages": [{
			"name": "test:demo@0.1.0",
			"interfaces": {},
			"worlds": { "demo": 0 }
		}]
	})");
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", error);
	REQUIRE(error.empty());
	REQUIRE(schema.imports.size() == 1u);
	REQUIRE(schema.imports[0].raw.at("type").at("params")[0].at("wit"));
	REQUIRE(schema.imports[0].raw.at("abi").at("params")[0].at("type")
			== "i32");
	REQUIRE(schema.imports[0].raw.at("abi").at("results")[0].at("type")
			== "i32");
	const std::string header = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CppTypedefs);
	REQUIRE(header.find("struct wasmjit_wit_point;") != std::string::npos);
	REQUIRE(header.find("struct wasmjit_wit_point {") != std::string::npos);
	REQUIRE(header.find("int32_t x;") != std::string::npos);
	// `echo(value: point)` takes and returns the named record, so the parameter's
	// spelling is the record pointer — the name is what this case is about.
	REQUIRE(header.find("wasmjit_wit_point* value") != std::string::npos);
	REQUIRE(schema.types[0].at("witLayout").at("size") == 4);
	REQUIRE(schema.types[0].at("witLayout").at("align") == 4);
	REQUIRE(schema.types[5].at("witOwnership").at("kind") == "resource");
	REQUIRE(schema.types[5].at("witOwnership").at("drop") == "resource.drop");
	REQUIRE(schema.types[6].at("witOwnership").at("kind") == "own");
	REQUIRE(schema.types[6].at("witOwnership").at("table")
			== "component-runtime");
	REQUIRE(header.find("enum class wasmjit_wit_color") != std::string::npos);
	REQUIRE(header.find("red = 0") != std::string::npos);
	REQUIRE(header.find("using wasmjit_wit_perms = std::uint32_t;")
			!= std::string::npos);
	REQUIRE(header.find("wasmjit_wit_perms_read") != std::string::npos);
	REQUIRE(header.find("struct wasmjit_wit_list_u8 {") != std::string::npos);
	REQUIRE(header.find("uint8_t* data;") != std::string::npos);
	REQUIRE(header.find("struct wasmjit_wit_variant_point_point_empty {")
			!= std::string::npos);
	REQUIRE(header.find("Tag tag;") != std::string::npos);
	REQUIRE(header.find("wasmjit_wit_point* point;") != std::string::npos);
	REQUIRE(header.find("struct wasmjit_wit_descriptor {\n\tuint32_t handle;")
			!= std::string::npos);
	REQUIRE(header.find("struct wasmjit_wit_own_descriptor {\n\tuint32_t handle;")
			!= std::string::npos);
	REQUIRE(header.find("owned WIT handle; release through the backend")
			!= std::string::npos);
}

TEST_CASE("WIT functions carry canonical ABI flat signatures", "[bindings]")
{
	const Json wit = Json::parse(R"({
		"worlds": [{
			"name": "demo",
			"imports": {
				"takes-string": { "function": {
					"name": "takes-string",
					"params": [{ "name": "s", "type": "string" }] } },
				"returns-string": { "function": {
					"name": "returns-string", "result": "string" } },
				"takes-record": { "function": {
					"name": "takes-record",
					"params": [{ "name": "p", "type": 0 }] } },
				"takes-variant": { "function": {
					"name": "takes-variant",
					"params": [{ "name": "v", "type": 1 }] } },
				"takes-mixed": { "function": {
					"name": "takes-mixed",
					"params": [{ "name": "v", "type": 2 }] } }
			},
			"exports": {},
			"package": 0
		}],
		"interfaces": [],
		"types": [
			{ "name": "point", "kind": { "record": {
				"fields": [
					{ "name": "x", "type": "s32" },
					{ "name": "y", "type": "s64" }] } } },
			{ "kind": { "variant": { "cases": [
				{ "name": "point", "type": 0 },
				{ "name": "empty", "type": null }] } } },
			{ "kind": { "variant": { "cases": [
				{ "name": "wide", "type": "f64" },
				{ "name": "narrow", "type": "i32" }] } } }
		],
		"packages": [{
			"name": "test:demo@0.1.0",
			"interfaces": {},
			"worlds": { "demo": 0 }
		}]
	})");
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", error);
	REQUIRE(error.empty());
	const auto find = [&](const std::string& name) -> const WASM::Bindings::Entry* {
		for (const auto& entry : schema.imports)
			if (entry.name == name)
				return &entry;
		return nullptr;
	};
	const auto stringParam = find("takes-string");
	REQUIRE(stringParam != nullptr);
	REQUIRE(stringParam->raw.at("type").at("params")[0].at("type") == "string");
	REQUIRE(stringParam->raw.at("abi").at("params").size() == 2u);
	REQUIRE(stringParam->raw.at("abi").at("params")[0].at("type") == "i32");
	REQUIRE(stringParam->raw.at("abi").at("params")[1].at("type") == "i32");
	REQUIRE(stringParam->raw.at("abi").at("paramLayout").at("size") == 8);
	REQUIRE(stringParam->raw.at("abi").at("paramLayout").at("align") == 4);

	const auto stringResult = find("returns-string");
	REQUIRE(stringResult != nullptr);
	REQUIRE(stringResult->raw.at("abi").at("results").size() == 1u);
	REQUIRE(stringResult->raw.at("abi").at("results")[0].at("type") == "i32");
	REQUIRE(stringResult->raw.at("abi").at("indirectResults") == true);
	REQUIRE(stringResult->raw.at("abi").at("resultLayout").at("size") == 8);
	REQUIRE(stringResult->raw.at("abi").at("resultLayout").at("align") == 4);

	const auto record = find("takes-record");
	REQUIRE(record != nullptr);
	REQUIRE(record->raw.at("abi").at("params").size() == 2u);
	REQUIRE(record->raw.at("abi").at("params")[0].at("type") == "i32");
	REQUIRE(record->raw.at("abi").at("params")[1].at("type") == "i64");
	REQUIRE(record->raw.at("abi").at("paramLayout").at("size") == 16);
	REQUIRE(record->raw.at("abi").at("paramLayout").at("align") == 8);

	const auto variant = find("takes-variant");
	REQUIRE(variant != nullptr);
	REQUIRE(variant->raw.at("abi").at("params").size() == 3u);
	REQUIRE(variant->raw.at("abi").at("params")[0].at("type") == "i32");
	REQUIRE(variant->raw.at("abi").at("params")[1].at("type") == "i32");
	REQUIRE(variant->raw.at("abi").at("params")[2].at("type") == "i64");
	REQUIRE(variant->raw.at("abi").at("paramLayout").at("size") == 24);
	REQUIRE(variant->raw.at("abi").at("paramLayout").at("align") == 8);

	const auto mixed = find("takes-mixed");
	REQUIRE(mixed != nullptr);
	REQUIRE(mixed->raw.at("abi").at("params").size() == 2u);
	REQUIRE(mixed->raw.at("abi").at("params")[0].at("type") == "i32");
	REQUIRE(mixed->raw.at("abi").at("params")[1].at("type") == "i64");
	REQUIRE(mixed->raw.at("abi").at("paramLayout").at("size") == 16);
	REQUIRE(mixed->raw.at("abi").at("paramLayout").at("align") == 8);
}

TEST_CASE("WIT interface functions use qualified package modules", "[bindings]")
{
	const Json wit = Json::parse(R"({
		"worlds": [{
			"name": "demo",
			"imports": { "interface-0": { "interface": { "id": 0 } } },
			"exports": {},
			"package": 0
		}],
		"interfaces": [{
			"name": "types",
			"package": 0,
			"functions": { "[method]descriptor.get-type": {
				"name": "[method]descriptor.get-type",
				"kind": { "async-method": 0 },
				"params": [{ "name": "self", "type": "u32" }],
				"result": "u32" } }
		}],
		"types": [],
		"packages": [{
			"name": "wasi:filesystem@0.3.2",
			"interfaces": { "types": 0 },
			"worlds": {}
		}]
	})");
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", error);
	REQUIRE(error.empty());
	REQUIRE(schema.imports.size() == 1u);
	REQUIRE(schema.imports[0].module == "wasi:filesystem/types@0.3.2");

	std::string rejectedError;
	const auto rejected = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", rejectedError,
			WASM::Bindings::WitAsyncPolicy::Reject);
	REQUIRE(rejected.imports.empty());
	REQUIRE(rejectedError.find("--wit-reject-async") != std::string::npos);
	const std::string header = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CHeader);
	REQUIRE(header.find("WASMJIT_WASM_IMPORT(\"wasi:filesystem/types@0.3.2\"")
			!= std::string::npos);
	REQUIRE(header.find("wasmjit_wasi_filesystem_types_0_3_2")
			!= std::string::npos);
	REQUIRE(header.find("WIT async functions are emitted as synchronous")
			!= std::string::npos);
}

TEST_CASE("WIT selects the world package and keeps stability", "[bindings]")
{
	const Json wit = Json::parse(R"({
		"worlds": [{
			"name": "demo",
			"imports": { "versioned": { "function": {
				"name": "versioned",
				"kind": "freestanding",
				"params": [],
				"result": "u32",
				"stability": { "stable": { "since": "0.3.0" } } } } },
			"exports": {},
			"package": 1
		}],
		"interfaces": [],
		"types": [],
		"packages": [
			{ "name": "other:pkg@0.1.0", "interfaces": {}, "worlds": {} },
			{ "name": "test:demo@0.1.0", "interfaces": {},
			  "worlds": { "demo": 0 } }
		]
	})");
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "", "demo", error,
			WASM::Bindings::WitAsyncPolicy::AllowSynchronous,
			"test:demo@0.1.0");
	REQUIRE(error.empty());
	REQUIRE(schema.module == "test:demo@0.1.0");
	REQUIRE(schema.imports.size() == 1u);
	REQUIRE(schema.imports[0].raw.at("stability").at("stable").at("since")
			== "0.3.0");
	REQUIRE(schema.imports[0].raw.at("abi").at("results")[0].at("type")
			== "i32");

	WASM::Bindings::WitModel model;
	REQUIRE(WASM::Bindings::parseWitModel(wit, model, error));
	REQUIRE(error.empty());
	REQUIRE(model.packages.size() == 2u);
	REQUIRE(model.worlds.size() == 1u);
	REQUIRE(model.worlds[0].package == 1);
	REQUIRE(model.packages[1].name == "test:demo@0.1.0");

	std::string missingError;
	const auto missing = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "", "demo", missingError,
			WASM::Bindings::WitAsyncPolicy::AllowSynchronous, "other:pkg@0.1.0");
	REQUIRE(missing.imports.empty());
	REQUIRE(missingError.find("other:pkg@0.1.0") != std::string::npos);
}

TEST_CASE("WIT named types are qualified by their owning interface", "[bindings]")
{
	const Json wit = Json::parse(R"({
		"worlds": [{
			"name": "demo",
			"imports": { "interface-0": { "interface": { "id": 0 } } },
			"exports": {},
			"package": 0
		}],
		"interfaces": [{
			"name": "types",
			"package": 0,
			"functions": { "echo": {
				"name": "echo",
				"params": [{ "name": "p", "type": 0 }],
				"result": 0 } }
		}],
		"types": [{
			"name": "point",
			"owner": { "interface": 0 },
			"kind": { "record": { "fields": [
				{ "name": "x", "type": "s32" }] } }
		}],
		"packages": [{
			"name": "wasi:filesystem@0.3.2",
			"interfaces": { "types": 0 },
			"worlds": {}
		}]
	})");
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", error);
	REQUIRE(error.empty());
	const std::string header = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CppTypedefs);
	REQUIRE(header.find("struct wasmjit_wit_wasi_filesystem_types_0_3_2_point {")
			!= std::string::npos);
	REQUIRE(header.find("wasmjit_wit_wasi_filesystem_types_0_3_2_point")
			!= std::string::npos);

	WASM::Bindings::WitModel model;
	REQUIRE(WASM::Bindings::parseWitModel(wit, model, error));
	REQUIRE(error.empty());
	REQUIRE(model.interfaces.size() == 1u);
	REQUIRE(model.interfaces[0].functions.size() == 1u);
	REQUIRE(model.types.size() == 1u);
	REQUIRE(model.types[0].ownerInterface == 0);
	REQUIRE(model.types[0].ownerPackage == 0);
	REQUIRE(model.types[0].displayName == "wasi_filesystem_types_0_3_2_point");
}

TEST_CASE("every target declares named WIT types", "[bindings]")
{
	const Json wit = Json::parse(R"({
		"worlds": [{
			"name": "demo",
			"imports": { "echo": { "function": {
				"name": "echo",
				"params": [{ "name": "p", "type": 0 }],
				"result": 0 } } },
			"exports": {},
			"package": 0
		}],
		"interfaces": [],
		"types": [{
			"name": "point",
			"kind": { "record": { "fields": [
				{ "name": "x", "type": "s32" }] } }
		}],
		"packages": [{
			"name": "test:demo@0.1.0",
			"interfaces": {},
			"worlds": { "demo": 0 }
		}]
	})");
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "demo", "demo", error);
	REQUIRE(error.empty());
	for (const auto language : {
			WASM::Bindings::Language::CHeader,
			WASM::Bindings::Language::Rust,
			WASM::Bindings::Language::TypeScript,
			WASM::Bindings::Language::AssemblyScript,
			WASM::Bindings::Language::Zig,
			WASM::Bindings::Language::Kotlin,
			WASM::Bindings::Language::MoonBit,
			WASM::Bindings::Language::Go,
			WASM::Bindings::Language::Dart,
			WASM::Bindings::Language::Java,
			WASM::Bindings::Language::CSharp,
			WASM::Bindings::Language::FSharp,
			WASM::Bindings::Language::Python,
			WASM::Bindings::Language::Ruby,
			WASM::Bindings::Language::Lua,
			WASM::Bindings::Language::Swift,
			WASM::Bindings::Language::DLang,
			WASM::Bindings::Language::Nim,
			WASM::Bindings::Language::Crystal,
			WASM::Bindings::Language::Ada,
			WASM::Bindings::Language::Pascal,
			WASM::Bindings::Language::Haskell,
			WASM::Bindings::Language::CppTypedefs }) {
		INFO(WASM::Bindings::languageName(language));
		const std::string output = WASM::Bindings::emit(schema, language);
		REQUIRE((output.find("wasmjit_wit_point") != std::string::npos
				|| output.find("WasmjitWitPoint") != std::string::npos));
	}
}

TEST_CASE("WIT output is accepted by wasm-tools", "[bindings]")
{
	const Json wit = Json::parse(R"({
		"worlds": [{
			"name": "demo",
			"imports": { "echo": { "function": {
				"name": "echo",
				"params": [{ "name": "p", "type": 0 }],
				"result": 0 } } },
			"exports": {},
			"package": 0
		}],
		"interfaces": [],
		"types": [
			{ "name": "point", "kind": { "record": { "fields": [
				{ "name": "x", "type": "s32" }] } } },
			{ "name": "descriptor", "kind": "resource" },
			{ "kind": { "handle": { "own": 1 } } },
			{ "kind": { "stream": "u8" } },
			{ "kind": { "future": "u32" } }
		],
		"packages": [{
			"name": "test:demo@0.1.0",
			"interfaces": {},
			"worlds": { "demo": 0 }
		}]
	})");
	std::string error;
	const auto schema = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "test:demo@0.1.0", "demo", error);
	REQUIRE(error.empty());
	const std::string emitted = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::Wit);
	REQUIRE(emitted.find("package test:demo@0.1.0;") != std::string::npos);
	REQUIRE(emitted.find("world demo") != std::string::npos);
	REQUIRE(emitted.find("record point") != std::string::npos);
	REQUIRE(emitted.find("resource descriptor") != std::string::npos);
	REQUIRE(emitted.find("own<descriptor>") != std::string::npos);
	REQUIRE(emitted.find("stream<u8>") != std::string::npos);
	REQUIRE(emitted.find("future<u32>") != std::string::npos);
	REQUIRE(emitted.find("echo: func") != std::string::npos);
	if (!haveTool("command -v wasm-tools > /dev/null 2>&1")) {
		WARN("wasm-tools not found; skipped the WIT output parse check");
		return;
	}
	const std::filesystem::path path = std::filesystem::temp_directory_path()
			/ "wasmjit-wit-emitted.wit";
	{
		std::ofstream out(path, std::ios::binary);
		out << emitted;
	}
	const std::filesystem::path log = std::filesystem::temp_directory_path()
			/ "wasmjit-wit-emitted.log";
	const std::string command = "wasm-tools component wit '"
			+ path.string() + "' --json > '"
			+ (std::filesystem::temp_directory_path()
					/ "wasmjit-wit-emitted.json").string()
			+ "' 2> '" + log.string() + "'";
	const int status = std::system(command.c_str());
	REQUIRE(status == 0);
	std::filesystem::remove(path);
	std::filesystem::remove(log);
}

TEST_CASE("WITX preserves Preview 1 parameter names", "[bindings]")
{
	const std::string source = R"(
(module $wasi_snapshot_preview1
  (@interface func (export "fd_write")
    (param $fd $fd)
    (param $iovs (@witx pointer $iovec))
    (param $iovs_len $size)
    (param $nwritten (@witx pointer $size))
    (result $error $errno)
  )
)
)";
	const std::filesystem::path path = std::filesystem::temp_directory_path()
			/ "wasmjit-test-preview1.witx";
	{
		std::ofstream out(path, std::ios::binary);
		out << source;
	}
	std::string error;
	const auto schema = WASM::Bindings::witxSchemaFromFile(path,
			WASM::Bindings::Direction::Complete, "", error);
	REQUIRE(error.empty());
	REQUIRE(schema.module == "wasi_snapshot_preview1");
	REQUIRE(schema.imports.size() == 1u);
	REQUIRE(schema.imports[0].name == "fd_write");
	REQUIRE(schema.imports[0].argNames.size() == 4u);
	REQUIRE(schema.imports[0].argNames[0] == "fd");
	REQUIRE(schema.imports[0].argNames[1] == "iovs");
	REQUIRE(schema.imports[0].argNames[2] == "iovs_len");
	REQUIRE(schema.imports[0].argNames[3] == "nwritten");
	const std::string emitted = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::Witx);
	REQUIRE(emitted.find("$iovs") != std::string::npos);
	REQUIRE(emitted.find("$nwritten") != std::string::npos);
	const std::filesystem::path roundtrip = std::filesystem::temp_directory_path()
			/ "wasmjit-test-preview1-roundtrip.witx";
	{
		std::ofstream out(roundtrip, std::ios::binary);
		out << emitted;
	}
	const auto reparsed = WASM::Bindings::witxSchemaFromFile(roundtrip,
			WASM::Bindings::Direction::Complete, "", error);
	REQUIRE(error.empty());
	REQUIRE(reparsed.imports.size() == 1u);
	REQUIRE(reparsed.imports[0].argNames.size() == 4u);
	REQUIRE(reparsed.imports[0].argNames[1] == "iovs");

	const std::filesystem::path malformed = std::filesystem::temp_directory_path()
			/ "wasmjit-test-malformed.witx";
	{
		std::ofstream out(malformed, std::ios::binary);
		out << ")";
	}
	const auto rejected = WASM::Bindings::witxSchemaFromFile(malformed,
			WASM::Bindings::Direction::Complete, "", error);
	REQUIRE(rejected.imports.empty());
	REQUIRE(!error.empty());
	std::filesystem::remove(path);
	std::filesystem::remove(roundtrip);
	std::filesystem::remove(malformed);
}

TEST_CASE("WIT ABI conformance matrix", "[bindings]")
{
	if (!haveTool("command -v wasm-tools > /dev/null 2>&1")) {
		WARN("wasm-tools not found; skipped the WIT ABI matrix");
		return;
	}
	const char* fixtures[] = {
		"component_runtime_import",
		"component_runtime_string",
		"component_canonopts",
		"component_runtime_import_lower_string",
	};
	for (const char* fixture : fixtures) {
		INFO(fixture);
		WASM::Component component;
		loadTestComponent(fixture, component);
		const Json g2h = WASM::Bindings::componentSchema(component,
				testStore().types(), WASM::Bindings::Direction::GuestToHost,
				fixture);
		const Json h2g = WASM::Bindings::componentSchema(component,
				testStore().types(), WASM::Bindings::Direction::HostToGuest,
				fixture);
		const std::filesystem::path witPath = std::filesystem::temp_directory_path()
				/ ("wasmjit-abi-" + std::string(fixture) + ".json");
		const std::filesystem::path logPath = std::filesystem::temp_directory_path()
				/ ("wasmjit-abi-" + std::string(fixture) + ".log");
		const std::string command = "wasm-tools component wit '"
				+ (std::filesystem::path(WASM_TEST_DIR)
						/ (std::string(fixture) + ".wasm")).string()
				+ "' --json > '" + witPath.string() + "' 2> '"
				+ logPath.string() + "'";
		INFO(readText(logPath));
		REQUIRE(std::system(command.c_str()) == 0);
		const Json wit = Json::parse(readText(witPath));
		std::string error;
		const auto derived = WASM::Bindings::witSchema(wit,
				WASM::Bindings::Direction::Complete, fixture, "", error);
		REQUIRE(error.empty());
		std::size_t compared = 0;
		const auto compare = [&](const Json& source,
				const std::vector<WASM::Bindings::Entry>& target) {
			for (const Json& entry : source) {
				if (entry.value("kind", "") != "function"
						|| !entry.contains("abi"))
					continue;
				const WASM::Bindings::Entry* match = nullptr;
				for (const auto& candidate : target)
					if (candidate.name == entry.value("name", ""))
						match = &candidate;
				if (match == nullptr || !match->raw.contains("abi"))
					continue;
				const Json& left = entry.at("abi");
				const Json& right = match->raw.at("abi");
				REQUIRE(left.at("params").size() == right.at("params").size());
				REQUIRE(left.at("results").size() == right.at("results").size());
				REQUIRE(left.at("indirectParams") == right.at("indirectParams"));
				REQUIRE(left.at("indirectResults") == right.at("indirectResults"));
				for (std::size_t i = 0; i < left.at("params").size(); ++i)
					REQUIRE(left.at("params")[i].at("type")
							== right.at("params")[i].at("type"));
				for (std::size_t i = 0; i < left.at("results").size(); ++i)
					REQUIRE(left.at("results")[i].at("type")
							== right.at("results")[i].at("type"));
				++compared;
			}
		};
		compare(g2h.at("imports"), derived.imports);
		compare(h2g.at("exports"), derived.exports);
		REQUIRE(compared > 0u);
		std::filesystem::remove(witPath);
		std::filesystem::remove(logPath);
	}
}

TEST_CASE("WIT JSON names agree with the component schema", "[bindings]")
{
	if (!haveTool("command -v wasm-tools > /dev/null 2>&1")) {
		WARN("wasm-tools not found; skipped the WIT-name cross-check");
		return;
	}

	WASM::Component component;
	loadTestComponent("component_runtime_import", component);
	const Json g2h = WASM::Bindings::componentSchema(component, testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "component_runtime_import");
	const Json h2g = WASM::Bindings::componentSchema(component, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "component_runtime_import");
	REQUIRE_FALSE(g2h.at("imports").empty());
	REQUIRE(g2h.at("exports").empty());
	REQUIRE(h2g.at("imports").empty());
	REQUIRE_FALSE(h2g.at("exports").empty());

	const std::filesystem::path dir = bindingsDir();
	const std::filesystem::path witPath = dir / "component_runtime_import.wit.json";
	const std::filesystem::path log = dir / "wasm-tools-wit.log";
	const std::string command = "wasm-tools component wit \""
			+ (std::filesystem::path(WASM_TEST_DIR) / "component_runtime_import.wasm").string()
			+ "\" --json > \"" + witPath.string() + "\" 2> \"" + log.string() + "\"";
	INFO(readText(log));
	REQUIRE(std::system(command.c_str()) == 0);

	const Json wit = Json::parse(readText(witPath));
	const Json& world = wit.at("worlds").at(0);
	const Json& witExport = world.at("exports").at("host-add").at("function");
	const Json* schemaExport = findByName(h2g.at("exports"), "host-add");
	REQUIRE(schemaExport != nullptr);
	REQUIRE(witExport.at("name").get<std::string>() == "host-add");
	REQUIRE(witExport.at("params").size()
			== schemaExport->at("type").at("params").size());
	for (std::size_t i = 0; i < witExport.at("params").size(); ++i) {
		REQUIRE(witExport.at("params")[i].at("name").get<std::string>()
				== schemaExport->at("type").at("params")[i].at("name").get<std::string>());
		REQUIRE(witExport.at("params")[i].at("type").get<std::string>()
				== schemaExport->at("type").at("params")[i].at("type").get<std::string>());
	}
	REQUIRE(witExport.at("result").get<std::string>()
			== schemaExport->at("type").at("results")[0].at("type").get<std::string>());

	const Json& witImport = world.at("imports").at("host-add").at("function");
	const Json* schemaImport = findByName(g2h.at("imports"), "host-add");
	REQUIRE(schemaImport != nullptr);
	REQUIRE(witImport.at("params").size()
			== schemaImport->at("type").at("params").size());
	REQUIRE(witImport.at("result").get<std::string>()
			== schemaImport->at("type").at("results")[0].at("type").get<std::string>());

	std::string witError;
	const auto derived = WASM::Bindings::witSchema(wit,
			WASM::Bindings::Direction::Complete, "component_runtime_import", "",
			witError);
	REQUIRE(witError.empty());
	const auto find = [](const auto& entries, const std::string& name) {
		for (const auto& entry : entries)
			if (entry.name == name)
				return &entry;
		return decltype(&entries.front())(nullptr);
	};
	const auto* derivedImport = find(derived.imports, "host-add");
	const auto* derivedExport = find(derived.exports, "host-add");
	REQUIRE(derivedImport != nullptr);
	REQUIRE(derivedExport != nullptr);
	REQUIRE(derivedImport->raw.at("abi").at("params").size()
			== schemaImport->at("abi").at("params").size());
	REQUIRE(derivedImport->raw.at("abi").at("params")[0].at("type")
			== schemaImport->at("abi").at("params")[0].at("type"));
	REQUIRE(derivedImport->raw.at("abi").at("results")[0].at("type")
			== schemaImport->at("abi").at("results")[0].at("type"));
	REQUIRE(derivedImport->raw.at("abi").at("indirectResults")
			== schemaImport->at("abi").at("indirectResults"));
	REQUIRE(derivedExport->raw.at("abi").at("params").size()
			== schemaExport->at("abi").at("params").size());
	REQUIRE(derivedExport->raw.at("abi").at("results")[0].at("type")
			== schemaExport->at("abi").at("results")[0].at("type"));
	REQUIRE(derivedExport->raw.at("abi").at("indirectResults")
			== schemaExport->at("abi").at("indirectResults"));
}

TEST_CASE("duplicate function entries are emitted once in every language",
		"[bindings]")
{
	const Json functionType = {
		{ "kind", "func" },
		{ "params", Json::array({ { { "type", "i32" } } }) },
		{ "results", Json::array({ { { "type", "i32" } } }) },
	};
	Json importEntry = {
		{ "kind", "function" },
		{ "module", "dup_host" },
		{ "name", "dup_import" },
		{ "type", functionType },
	};
	Json exportEntry = {
		{ "kind", "function" },
		{ "name", "dup_export" },
		{ "type", functionType },
	};

	Json base = {
		{ "schema", "wasmjit.binding/1" },
		{ "kind", "module" },
		{ "direction", "all" },
		{ "module", "dup_module" },
		{ "types", Json::array() },
	};
	Json single = base;
	single["imports"] = Json::array({ importEntry });
	single["exports"] = Json::array({ exportEntry });
	Json duplicate = base;
	duplicate["imports"] = Json::array({ importEntry, importEntry });
	duplicate["exports"] = Json::array({ exportEntry, exportEntry });

	const WASM::Bindings::Language languages[] = {
		WASM::Bindings::Language::CHeader,
		WASM::Bindings::Language::Rust,
		WASM::Bindings::Language::TypeScript,
		WASM::Bindings::Language::AssemblyScript,
		WASM::Bindings::Language::Zig,
		WASM::Bindings::Language::Kotlin,
		WASM::Bindings::Language::MoonBit,
		WASM::Bindings::Language::Go,
		WASM::Bindings::Language::Dart,
		WASM::Bindings::Language::Java,
		WASM::Bindings::Language::CSharp,
		WASM::Bindings::Language::FSharp,
		WASM::Bindings::Language::Python,
		WASM::Bindings::Language::Ruby,
		WASM::Bindings::Language::Lua,
		WASM::Bindings::Language::Swift,
		WASM::Bindings::Language::DLang,
		WASM::Bindings::Language::Nim,
		WASM::Bindings::Language::Crystal,
		WASM::Bindings::Language::Ada,
		WASM::Bindings::Language::Pascal,
		WASM::Bindings::Language::Haskell,
		WASM::Bindings::Language::CppAdapter,
		WASM::Bindings::Language::CppTypedefs,
		WASM::Bindings::Language::CppSource,
	};
	for (const auto language : languages) {
		INFO(WASM::Bindings::languageName(language));
		REQUIRE(WASM::Bindings::emit(duplicate, language)
				== WASM::Bindings::emit(single, language));
	}
}

TEST_CASE("candidate schemas keep one entry per function name", "[bindings]")
{
	const WASM::Bindings::CandidateSignature candidate{
		"host_add", "int32_t",
		{ { "WASM::VMContext*", "context" }, { "int32_t", "a" } },
		"Adds one.",
	};
	const Json schema = WASM::Bindings::candidateSchema(
			{ candidate, candidate }, "g2h", "host_functions", "swig");
	REQUIRE(schema.at("imports").size() == 1u);
	REQUIRE(schema.at("imports")[0].at("name") == "host_add");
}



TEST_CASE("bindings spell a v128 parameter", "[bindings][v128][abi]")
{
	// Pins how the emitters spell a vector today, so moving a family to its own side
	// of the boundary becomes a visible, deliberate change rather than a silent one.
	// Per docs/backends/HISTORY.md the guest-facing emitters must end up on the
	// wasm spelling; this case records where they are now.
	const Json schema = WASM::Bindings::moduleSchema(loadTestModule("v128_param"),
testStore().types(), WASM::Bindings::Direction::HostToGuest, "v128_param");
	REQUIRE(!schema.is_null());

	const std::string header = WASM::Bindings::emit(schema, WASM::Bindings::Language::CHeader);
	const std::size_t lane = header.find("lane1");
	INFO(header);
	REQUIRE(lane != std::string::npos);
	// The parameter as declared, whatever the spelling: this assertion is the record.
	// Measured 2026-11-06. Today the guest-facing C header declares ONE
	// aggregate parameter whose type is the NATIVE pair -- neither the wasm
	// spelling (one wasm v128) nor the native one (two scalars). This pins the
	// current text so moving the header to the wasm side is a visible change.
	CHECK(header.find("wasmjit_lane1(wasm_v128 arg0)") != std::string::npos);
	CHECK(header.find("lane1") != std::string::npos);

	const std::string rust = WASM::Bindings::emit(schema, WASM::Bindings::Language::Rust);
	INFO(rust);
	// Guest-side Rust: one wasm vector parameter, not the native pair.
	CHECK(rust.find("wasm_v128") != std::string::npos);
	CHECK(rust.find("arg0_lo") == std::string::npos);

	// The WasmJIT-facing family is the mirror image: the native side must spell a
	// vector as TWO scalar words, because the embedder copies this declaration and
	// calls the compiled code directly (study §16). An aggregate here would be
	// passed by reference on MS x64.
	const std::string cpp = WASM::Bindings::emit(schema, WASM::Bindings::Language::CppTypedefs);
	INFO(cpp);
	CHECK(cpp.find("uint64_t arg0_lo, uint64_t arg0_hi") != std::string::npos);
	CHECK(cpp.find("wasm_v128 arg0") == std::string::npos);

	// Every other guest-facing emitter must obey the same boundary rule: no
	// language may adopt WasmJIT's native pair split, because the conversion
	// happens at instantiation (study §16). Asserting an absence keeps this
	// honest for emitters that refuse a schema rather than emit it.
	for (const WASM::Bindings::Language language : {
		WASM::Bindings::Language::TypeScript, WASM::Bindings::Language::AssemblyScript,
		WASM::Bindings::Language::Zig, WASM::Bindings::Language::Kotlin,
		WASM::Bindings::Language::MoonBit, WASM::Bindings::Language::Go,
		WASM::Bindings::Language::Dart, WASM::Bindings::Language::Java,
		WASM::Bindings::Language::CSharp, WASM::Bindings::Language::FSharp,
		WASM::Bindings::Language::Python, WASM::Bindings::Language::Ruby}) {
		const std::string text = WASM::Bindings::emit(schema, language);
		INFO("language index " << static_cast<int>(language));
		CHECK(text.find("arg0_lo") == std::string::npos);
	}
}
namespace {

//! A `wasmjit.binding/2` schema: the object section is input the generator
//! consumes and never produces (docs/proposals/SYNTHETIC_OBJECTS.md A.14.5).
Json objectSchema()
{
	return Json::parse(R"({
		"schema": "wasmjit.binding/2",
		"direction": "all",
		"module": "demo",
		"objects": [
			{ "name": "demo.Widget", "version": 1,
			  "parent": "demo.Base",
			  "interfaces": ["demo.Sized"],
			  "fields": [
				{ "name": "x", "type": "i32" },
				{ "name": "y", "type": "i32", "mutable": true },
				{ "name": "label", "type": { "type": "ref", "heap": "extern" } },
				{ "name": "target", "type": { "type": "ref", "heap": "defined", "name": "demo.Node" }, "mutable": true }
			  ],
			  "properties": [
				{ "name": "status", "type": "i32", "get": "get_status", "set": "set_status" },
				{ "name": "caption", "type": "i32", "get": "compute_caption" },
				{ "name": "preview", "type": { "type": "ref", "heap": "defined", "name": "demo.Node" }, "get": "get_preview" }
			  ],
			  "methods": [
				{ "name": "resize", "argNames": ["width", "height"], "params": ["i32", "i32"], "results": [] },
				{ "name": "area", "params": [], "results": ["i32"] },
				{ "name": "attach", "argNames": ["target"], "params": [ { "type": "ref", "heap": "defined", "name": "demo.Node" } ], "results": [] },
				{ "name": "blit", "argNames": ["lanes"], "params": ["v128"], "results": [] },
				{ "name": "get_status", "params": [], "results": ["i32"] },
				{ "name": "set_status", "params": ["i32"], "results": [] },
				{ "name": "compute_caption", "params": [], "results": ["i32"] },
				{ "name": "get_preview", "params": [], "results": [ { "type": "ref", "heap": "defined", "name": "demo.Node" } ] }
			  ],
			  "constructors": [ { "name": "create", "argNames": ["width", "height"], "params": ["i32", "i32"] },
			                    { "name": "open", "argNames": ["slot"], "params": ["i64"] } ] },
			{ "name": "demo.Node", "version": 1,
			  "fields": [ { "name": "kind", "type": "i8" } ],
			  "constructors": [ { "name": "spawn", "params": [] } ] },
			{ "name": "demo.Widget", "version": 2,
			  "fields": [ { "name": "x", "type": "i32" } ] }
		],
		"imports": [], "exports": []
	})");
}

} // namespace

TEST_CASE("a v2 schema validates object offsets against the canonical layout", "[bindings][objects]")
{
	const WASM::Bindings::Schema schema = WASM::Bindings::Schema::fromJson(objectSchema());
	CHECK(schema.id == "wasmjit.binding/2");
	REQUIRE(schema.objects.size() == 3u);

	const WASM::Bindings::ObjectDescriptor& widget = schema.objects[0];
	CHECK(widget.name == "demo.Widget");
	CHECK(widget.version == 1u);
	CHECK(widget.parent == "demo.Base");
	REQUIRE(widget.interfaces.size() == 1u);
	CHECK(widget.interfaces[0] == "demo.Sized");
	REQUIRE(widget.fields.size() == 4u);
	// Header 16: i32, i32, a pointer-aligned reference at 24 and another at 32.
	CHECK(widget.fields[0].offset == 16u);
	CHECK(widget.fields[1].offset == 20u);
	CHECK(widget.fields[1].bytes == 4u);
	CHECK(widget.fields[1].mutableField);
	CHECK(widget.fields[2].offset == 24u);
	CHECK(widget.fields[2].bytes == 8u);
	CHECK(widget.fields[2].isReference);
	CHECK(widget.fields[3].offset == 32u);
	CHECK(widget.fields[3].isReference);
	CHECK(widget.size == 40u);
	// Two properties and their three accessors are declared as methods too: an
	// accessor is a function of the object, not a second kind of member.
	REQUIRE(widget.properties.size() == 3u);
	CHECK(widget.properties[0].name == "status");
	CHECK(widget.properties[0].getter == "get_status");
	CHECK(widget.properties[0].setter == "set_status");
	CHECK(widget.properties[0].mutableProperty);
	CHECK(widget.properties[1].name == "caption");
	CHECK(widget.properties[1].setter.empty());
	CHECK_FALSE(widget.properties[1].mutableProperty);
	// A property may itself be a typed handle, not only a scalar.
	CHECK(widget.properties[2].type.kind == WASM::Bindings::ValueKind::Ref);
	CHECK(widget.properties[2].type.heap == WASM::Bindings::HeapKind::Defined);
	CHECK(widget.properties[2].type.name == "demo.Node");
	REQUIRE(widget.methods.size() == 8u);
	CHECK(widget.methods[0].params.size() == 2u);
	CHECK(widget.methods[2].params.size() == 1u);
	// A packed field is one byte and the object is `header + 1`.
	CHECK(schema.objects[1].fields[0].bytes == 1u);
	CHECK(schema.objects[1].size == 17u);
	// Two versions of one name are two descriptors, not a dedup.
	CHECK(schema.objects[2].version == 2u);
	CHECK(schema.objects[2].size == 20u);

	// The interchange form carries the derived facts, not the asserted ones.
	const Json out = schema.toJson();
	CHECK(out.at("schema") == "wasmjit.binding/2");
	REQUIRE(out.at("objects").size() == 3u);
	CHECK(out.at("objects")[0].at("size") == 40u);
	CHECK(out.at("objects")[0].at("fields")[1].at("offset") == 20u);
	CHECK(out.at("objects")[0].at("fields")[2].at("reference") == true);
	CHECK(out.at("objects")[0].at("methods")[0].at("name") == "resize");

	// A `/1` schema keeps its version and gains no empty section.
	const Json one = Json::parse(R"({
		"schema": "wasmjit.binding/1", "direction": "g2h", "module": "m",
		"imports": [], "exports": [] })");
	const Json oneOut = WASM::Bindings::Schema::fromJson(one).toJson();
	CHECK(oneOut.at("schema") == "wasmjit.binding/1");
	CHECK_FALSE(oneOut.contains("objects"));
}

TEST_CASE("the object section is refused when its version or layout disagrees", "[bindings][objects]")
{
	using WASM::Bindings::Schema;
	const auto failure = [](const Json& input) {
		try {
			(void)Schema::fromJson(input);
		} catch (const std::runtime_error& error) {
			return std::string(error.what());
		}
		return std::string();
	};

	Json unknown = objectSchema();
	unknown["schema"] = "wasmjit.binding/3";
	CHECK(failure(unknown).find("wasmjit.binding/2") != std::string::npos);

	Json mislabelled = objectSchema();
	mislabelled["schema"] = "wasmjit.binding/1";
	CHECK(failure(mislabelled).find("requires wasmjit.binding/2") != std::string::npos);

	Json shifted = objectSchema();
	shifted["objects"][0]["fields"][1]["offset"] = 8;
	CHECK(failure(shifted).find("canonical offset 20") != std::string::npos);

	Json duplicate = objectSchema();
	duplicate["objects"][2]["version"] = 1;
	CHECK(failure(duplicate).find("declared twice") != std::string::npos);

	Json badType = objectSchema();
	badType["objects"][1]["fields"][0]["type"] = "string";
	CHECK(failure(badType).find("not a Wasm storage type") != std::string::npos);
}

TEST_CASE("a property is an accessor pair, not a stored slot", "[bindings][objects]")
{
	using WASM::Bindings::Schema;
	const Json source = objectSchema();
	const Schema schema = Schema::fromJson(source);
	const WASM::Bindings::ObjectDescriptor& widget = schema.objects[0];
	REQUIRE(widget.properties.size() == 3u);

	// A property owns no bytes: dropping the section changes no offset and no
	// size, because `layoutHostType` never saw it.
	Json stripped = source;
	stripped["objects"][0].erase("properties");
	const Schema plain = Schema::fromJson(stripped);
	REQUIRE(plain.objects.size() == 3u);
	REQUIRE(plain.objects[0].fields.size() == widget.fields.size());
	CHECK(plain.objects[0].size == widget.size);
	for (std::size_t index = 0; index < widget.fields.size(); ++index) {
		CHECK(plain.objects[0].fields[index].offset == widget.fields[index].offset);
		CHECK(plain.objects[0].fields[index].bytes == widget.fields[index].bytes);
	}

	// The interchange form keeps the accessor pair, and only when there is one.
	const Json out = schema.toJson();
	REQUIRE(out.at("objects")[0].contains("properties"));
	const Json& properties = out.at("objects")[0].at("properties");
	REQUIRE(properties.size() == 3u);
	CHECK(properties[0].at("name") == "status");
	CHECK(properties[0].at("get") == "get_status");
	CHECK(properties[0].at("set") == "set_status");
	CHECK_FALSE(properties[1].contains("set"));
	CHECK_FALSE(out.at("objects")[1].contains("properties"));
}

TEST_CASE("a property that cannot be bound is refused at load", "[bindings][objects]")
{
	using WASM::Bindings::Schema;
	const auto failure = [](const Json& input) {
		try {
			(void)Schema::fromJson(input);
		} catch (const std::runtime_error& error) {
			return std::string(error.what());
		}
		return std::string();
	};

	// No getter: a member you can only write is a method with a side effect.
	Json noGetter = objectSchema();
	noGetter["objects"][0]["properties"][0].erase("get");
	CHECK(failure(noGetter).find("missing `get`") != std::string::npos);

	// A property is not a slot, so an offset is a field written the wrong way.
	Json offset = objectSchema();
	offset["objects"][0]["properties"][0]["offset"] = 8;
	CHECK(failure(offset).find("no storage offset") != std::string::npos);

	// `mutable` follows the setter; asserting it without one is a contradiction.
	Json readOnly = objectSchema();
	readOnly["objects"][0]["properties"][1]["mutable"] = true;
	CHECK(failure(readOnly).find("`mutable` without `set`") != std::string::npos);

	// A field, a property and a method share one member namespace.
	Json fieldName = objectSchema();
	fieldName["objects"][0]["properties"][0]["name"] = "x";
	CHECK(failure(fieldName).find("same name as a field") != std::string::npos);

	Json methodName = objectSchema();
	methodName["objects"][0]["properties"][0]["name"] = "area";
	CHECK(failure(methodName).find("same name as a method") != std::string::npos);

	// An accessor is a function of the object, so `methods` must declare it...
	Json undeclared = objectSchema();
	undeclared["objects"][0]["properties"][0]["get"] = "read_status";
	CHECK(failure(undeclared).find("is not declared in `methods`")
			!= std::string::npos);

	// ... and the declared signature must be the one the property implies.
	Json wrongGetter = objectSchema();
	wrongGetter["objects"][0]["methods"][4]["results"] = Json::array({ "i64" });
	CHECK(failure(wrongGetter).find("must take no parameters and return i32")
			!= std::string::npos);

	Json wrongSetter = objectSchema();
	wrongSetter["objects"][0]["methods"][5]["params"] = Json::array();
	CHECK(failure(wrongSetter).find("must take i32 and return nothing")
			!= std::string::npos);

	// Two properties of one name, or two properties under one accessor, are
	// both ambiguous: one member, one accessor.
	Json duplicate = objectSchema();
	const Json twice = duplicate["objects"][0]["properties"][0];
	duplicate["objects"][0]["properties"].push_back(twice);
	CHECK(failure(duplicate).find("is declared twice") != std::string::npos);

	Json shared = objectSchema();
	shared["objects"][0]["properties"][1]["get"] = "get_status";
	CHECK(failure(shared).find("already claimed by another property")
			!= std::string::npos);
}

// The handle surface owns one name in the descriptor's namespace — the lifetime
// entry point — and the C++ wrapper owns four in its own. The first is refused at
// load time because every generated artifact calls it by that name; the rest are
// projected as plain entry points, so the class stays unambiguous.
TEST_CASE("the wrapper's own member names are not taken by the descriptor",
		"[bindings][objects]")
{
	using WASM::Bindings::Schema;
	const Json source = Json::parse(R"({
		"schema": "wasmjit.binding/2", "direction": "all", "module": "demo",
		"objects": [ { "name": "demo.Trap", "version": 1,
			"fields": [],
			"properties": [ { "name": "handle", "type": "i32", "get": "get_handle" } ],
			"methods": [ { "name": "get_handle", "params": [], "results": ["i32"] },
			             { "name": "reset", "params": [], "results": [] },
			             { "name": "area", "params": [], "results": ["i32"] } ] } ],
		"imports": [], "exports": [] })");
	const Schema schema = Schema::fromJson(source);
	const std::string header =
			WASM::Bindings::emit(schema, WASM::Bindings::Language::CHeader);

	REQUIRE(header.find("class DemoTrapV1 {") != std::string::npos);
	// The wrapper's own members survive...
	CHECK(header.find("\twasmjit_object_demo_trap_v1_handle handle() const noexcept")
			!= std::string::npos);
	CHECK(header.find("\tvoid reset() noexcept") != std::string::npos);
	// ... the descriptor's members become notes plus entry points...
	CHECK(header.find("is not a member: the wrapper's own `handle` is")
			!= std::string::npos);
	CHECK(header.find("is not a member: the wrapper's own `reset` is")
			!= std::string::npos);
	CHECK(header.find("wasmjit_object_demo_trap_v1_prop_handle_get(")
			!= std::string::npos);
	CHECK(header.find("wasmjit_object_demo_trap_v1_reset(") != std::string::npos);
	// ... and a member that shadows nothing is still a member.
	CHECK(header.find("\tint32_t area()") != std::string::npos);

	// `release` is the one reserved at load time: it is the lifetime entry point.
	Json collision = source;
	collision["objects"][0]["methods"][2]["name"] = "release";
	try {
		(void)Schema::fromJson(collision);
		FAIL("a method named `release` must be refused");
	} catch (const std::runtime_error& error) {
		CHECK(std::string(error.what()).find("lifetime entry point")
				!= std::string::npos);
	}
}

// An `externref` object (docs/proposals/SYNTHETIC_OBJECTS.md §2.7, §4.13): the
// same descriptor tables over an opaque host reference, for a native type whose
// payload cannot be written as Wasm values. There is no slot and no canonical
// layout, the bridge owns the lifetime, and a declared constructor is the call
// that hands a reference over.
TEST_CASE("an externref object projects an opaque receiver in every target",
		"[bindings][objects]")
{
	using WASM::Bindings::Language;
	using WASM::Bindings::ObjectSelfKind;
	using WASM::Bindings::Schema;
	const Json source = Json::parse(R"({
		"schema": "wasmjit.binding/2", "direction": "all", "module": "demo",
		"objects": [ { "name": "demo.Session", "version": 1, "self": "externref",
			"properties": [ { "name": "status", "type": "i32", "get": "get_status" } ],
			"methods": [ { "name": "send", "argNames": ["bytes"],
			               "params": ["i32"], "results": ["i32"] },
			             { "name": "get_status", "params": [], "results": ["i32"] } ],
			"constructors": [ { "name": "open", "argNames": ["slot"], "params": ["i32"] } ] } ],
		"imports": [], "exports": [] })");
	const Schema schema = Schema::fromJson(source);
	REQUIRE(schema.objects.size() == 1u);
	CHECK(schema.objects.at(0).self == ObjectSelfKind::ExternRef);
	// No canonical layout exists, so the descriptor carries no size.
	CHECK(schema.objects.at(0).size == 0u);

	const std::string header = WASM::Bindings::emit(schema, Language::CHeader);
	// The typed accessor surface is the same shape as a handle object's...
	CHECK(header.find("typedef int32_t (*wasmjit_object_demo_session_v1_send_fn)"
			"(wasmjit_object_demo_session_v1* self, int32_t bytes);")
			!= std::string::npos);
	// ...a constructor hands a reference over instead of a slot...
	CHECK(header.find("wasmjit_object_demo_session_v1*"
			" wasmjit_object_demo_session_v1_open(int32_t slot);")
			!= std::string::npos);
	// ...and the slot surface is absent entirely: no handle, no size, no release.
	CHECK(header.find("wasmjit_object_demo_session_v1_handle") == std::string::npos);
	CHECK(header.find("_HANDLE_NONE") == std::string::npos);
	CHECK(header.find("wasmjit_object_demo_session_v1_release") == std::string::npos);
	CHECK(header.find("WASMJIT_OBJECT_DEMO_SESSION_V1_SIZE") == std::string::npos);
	// The C++ wrapper holds the reference and owns nothing, so it copies.
	CHECK(header.find("class DemoSessionV1 {") != std::string::npos);
	CHECK(header.find("static DemoSessionV1 open(int32_t slot)") != std::string::npos);
	CHECK(header.find("wasmjit_object_demo_session_v1* self() const noexcept")
			!= std::string::npos);
	CHECK(header.find("int32_t status() const") != std::string::npos);
	CHECK(header.find("void reset() noexcept") == std::string::npos);

	const std::string rust = WASM::Bindings::emit(schema, Language::Rust);
	CHECK(rust.find("pub type wasmjit_object_demo_session_v1_ref ="
			" *mut wasmjit_object_demo_session_v1;") != std::string::npos);
	CHECK(rust.find("_SIZE") == std::string::npos);

	const std::string wit = WASM::Bindings::emit(schema, Language::Wit);
	CHECK(wit.find("Opaque: the payload is host-private") != std::string::npos);
	CHECK(wit.find("resource demo-session-v1") != std::string::npos);

	const std::string kotlin = WASM::Bindings::emit(schema, Language::Kotlin);
	CHECK(kotlin.find("class DemoSessionV1(val ref: DemoSessionV1Ref)")
			!= std::string::npos);
	CHECK(kotlin.find("fun open(slot: Int): DemoSessionV1") != std::string::npos);
	// No `AutoCloseable`, so no `close()`: the bridge owns the reference.
	CHECK(kotlin.find("override fun close") == std::string::npos);
	CHECK(kotlin.find("AutoCloseable") == std::string::npos);

	// The shape round-trips, and both rules the wire cannot honour are refused.
	const Json round = schema.toJson();
	CHECK(round["objects"][0]["self"] == "externref");
	CHECK(round["objects"][0]["size"] == 0);

	const auto failure = [](const Json& candidate) {
		try {
			(void)Schema::fromJson(candidate);
		} catch (const std::runtime_error& error) {
			return std::string(error.what());
		}
		return std::string();
	};
	Json withFields = source;
	withFields["objects"][0]["fields"] =
			Json::parse(R"([ { "name": "x", "type": "i32" } ])");
	CHECK(failure(withFields).find("has no fields") != std::string::npos);
	Json unknownShape = source;
	unknownShape["objects"][0]["self"] = "value";
	CHECK(failure(unknownShape).find("must be") != std::string::npos);
}

// The generated artifacts compile: the declarations as C11, and the same header
// as C++23, where the opaque wrapper is a real class that copies rather than owns.
TEST_CASE("the generated externref object header compiles as C and C++",
		"[bindings][objects]")
{
	const Json source = Json::parse(R"({
		"schema": "wasmjit.binding/2", "direction": "all", "module": "demo",
		"objects": [ { "name": "demo.Session", "version": 1, "self": "externref",
			"methods": [ { "name": "send", "params": ["i32"], "results": ["i32"] },
			             { "name": "touch", "params": [], "results": [] } ],
			"constructors": [ { "name": "open", "params": [] } ] } ],
		"imports": [], "exports": [] })");
	const WASM::Bindings::Schema schema = WASM::Bindings::Schema::fromJson(source);
	const std::filesystem::path dir = bindingsDir() / "opaque_object";
	std::filesystem::create_directories(dir);
	const std::filesystem::path headerPath = dir / "demo.h";
	const std::filesystem::path cSourcePath = dir / "demo.c";
	const std::filesystem::path cppSourcePath = dir / "demo.cpp";
	const std::filesystem::path cLogPath = dir / "gcc.log";
	const std::filesystem::path cppLogPath = dir / "g++.log";
	{
		std::ofstream header(headerPath, std::ios::binary);
		header << WASM::Bindings::emit(schema, WASM::Bindings::Language::CHeader);
		std::ofstream cSource(cSourcePath, std::ios::binary);
		cSource << "#include \"demo.h\"\nint main(void) { return 0; }\n";
		std::ofstream cppSource(cppSourcePath, std::ios::binary);
		cppSource << "#include \"demo.h\"\n"
				   "int main()\n{\n"
				   "\tDemoSessionV1 session;\n"
				   "\tif (session) return 1;          // false: no reference yet\n"
				   "\tDemoSessionV1 copy = session;   // copies, does not own\n"
				   "\treturn copy.self() == nullptr ? 0 : 2;\n}\n";
	}

	if (haveTool("gcc --version > /dev/null 2>&1")) {
		const std::string command = "gcc -std=c11 -fsyntax-only -I\"" + dir.string()
				+ "\" \"" + cSourcePath.string() + "\" > \"" + cLogPath.string()
				+ "\" 2>&1";
		INFO(readText(cLogPath));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("gcc not found; skipped the generated C header compile check");
	}

	if (haveTool("g++ --version > /dev/null 2>&1")) {
		const std::string command = "g++ -std=c++2a -fsyntax-only -I\"" + dir.string()
				+ "\" \"" + cppSourcePath.string() + "\" > \"" + cppLogPath.string()
				+ "\" 2>&1";
		INFO(readText(cppLogPath));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("g++ not found; skipped the generated C++ wrapper compile check");
	}
}

// A constructor is an entry point with no `self`. It shares the method namespace,
// it cannot be spelled twice in one class, and its parameters are typed like any
// other — all checked at load time, because each failure would otherwise be a
// header that does not compile in the consumer's build.
TEST_CASE("a constructor descriptor is validated like an entry point",
		"[bindings][objects]")
{
	using WASM::Bindings::Schema;
	const auto failure = [](const Json& input) {
		try {
			(void)Schema::fromJson(input);
		} catch (const std::runtime_error& error) {
			return std::string(error.what());
		}
		return std::string();
	};

	// The interchange form keeps the table, and only when there is one.
	const Schema plenty = Schema::fromJson(objectSchema());
	const Json out = plenty.toJson();
	REQUIRE(out.at("objects")[0].contains("constructors"));
	CHECK(out.at("objects")[0].at("constructors")[0].at("name") == "create");
	CHECK(out.at("objects")[0].at("constructors")[0].at("params").size() == 2u);
	CHECK_FALSE(out.at("objects")[2].contains("constructors"));

	Json duplicate = objectSchema();
	duplicate["objects"][0]["constructors"].push_back(
			Json::parse(R"({ "name": "create", "params": ["f32"] })"));
	CHECK(failure(duplicate).find("declares constructor 'create' twice")
			!= std::string::npos);

	// `<type>_<name>` is one namespace: a method and a constructor cannot share it.
	Json methodName = objectSchema();
	methodName["objects"][0]["constructors"][0]["name"] = "area";
	CHECK(failure(methodName).find("and a method of the same name")
			!= std::string::npos);

	// Two names, one parameter list: one C++ constructor, so one is a mistake.
	Json sameSignature = objectSchema();
	sameSignature["objects"][0]["constructors"].push_back(
			Json::parse(R"({ "name": "make", "params": ["i32", "i32"] })"));
	CHECK(failure(sameSignature).find("same parameter list") != std::string::npos);

	// `release` is the object's lifetime entry point, whatever makes the object.
	Json release = objectSchema();
	release["objects"][0]["constructors"][0]["name"] = "release";
	CHECK(failure(release).find("lifetime entry point") != std::string::npos);

	// A name list may cover part of a signature but never more of it, and two
	// parameters of one name would be one identifier in every target language.
	Json shortNames = objectSchema();
	shortNames["objects"][1]["constructors"][0]["argNames"] =
			Json::array({ "value", "other" });
	CHECK(failure(shortNames).find("names 2 parameters but takes 0")
			!= std::string::npos);

	Json twins = objectSchema();
	twins["objects"][0]["methods"][0]["argNames"] = Json::array({ "size", "Size" });
	CHECK(failure(twins).find("a parameter name is unique within one signature")
			!= std::string::npos);

	Json receiver = objectSchema();
	receiver["objects"][0]["constructors"][0]["argNames"] =
			Json::array({ "self", "height" });
	CHECK(failure(receiver).find("`self` is the receiver") != std::string::npos);

	// A parameter is a type the engine can name: a constructor cannot smuggle in a
	// storage kind with no ABI, and `stringref` is one it can name.
	Json unknown = objectSchema();
	unknown["objects"][1]["constructors"][0]["params"] = Json::array({ "i128" });
	CHECK(failure(unknown).find("unknown param type") != std::string::npos);

	// A parameter the Kotlin façade cannot spell is omitted there with a note —
	// never guessed — while C, C++ and Rust still see the entry point.
	Json stringref = objectSchema();
	stringref["objects"][1]["constructors"][0]["params"] = Json::array({ "string" });
	const Schema softened = Schema::fromJson(stringref);
	CHECK(WASM::Bindings::emit(softened, WASM::Bindings::Language::CHeader)
					.find("wasmjit_object_demo_node_v1_handle "
						  "wasmjit_object_demo_node_v1_spawn(void* p0);")
			!= std::string::npos);
	CHECK(WASM::Bindings::emit(softened, WASM::Bindings::Language::Kotlin)
					.find("// `spawn`: a non-scalar parameter is not expressible"
						  " here")
			!= std::string::npos);
}

TEST_CASE("one object descriptor reaches the C, Rust and WIT emitters", "[bindings][objects]")
{
	const WASM::Bindings::Schema schema = WASM::Bindings::Schema::fromJson(objectSchema());

	const std::string header = WASM::Bindings::emitCHeader(schema);
	CHECK(header.find("Generated from the wasmjit.binding/2") != std::string::npos);
	CHECK(header.find("typedef struct wasmjit_object_meta_field {") != std::string::npos);
	CHECK(header.find("typedef struct wasmjit_object_demo_widget_v1 wasmjit_object_demo_widget_v1;")
			!= std::string::npos);
	CHECK(header.find("#define WASMJIT_OBJECT_DEMO_WIDGET_V1_VERSION 1u") != std::string::npos);
	CHECK(header.find("#define WASMJIT_OBJECT_DEMO_WIDGET_V1_SIZE 40u") != std::string::npos);
	CHECK(header.find("#define WASMJIT_OBJECT_DEMO_WIDGET_V1_FIELD_TARGET 32u") != std::string::npos);
	CHECK(header.find("wasmjit_object_demo_widget_v1_resize_fn") != std::string::npos);
	CHECK(header.find("wasmjit_object_demo_widget_v1_attach_fn)(wasmjit_object_demo_widget_v1* self,"
			" wasmjit_object_demo_node_v1* target)") != std::string::npos);
	CHECK(header.find("static const wasmjit_object_meta wasmjit_object_demo_widget_v1_meta")
			!= std::string::npos);
	CHECK(header.find("\"demo.Widget\", 1u, 40u, \"demo.Base\"") != std::string::npos);
	CHECK(header.find("\"target\", 32u, 8u, 1u, 1u") != std::string::npos);

	// A property is emitted as an export name plus an accessor signature: the
	// header says where the value comes from, never where it is stored.
	CHECK(header.find("typedef struct wasmjit_object_property_meta {")
			!= std::string::npos);
	CHECK(header.find("#define WASMJIT_OBJECT_DEMO_WIDGET_V1_PROP_STATUS_GET \"get_status\"")
			!= std::string::npos);
	CHECK(header.find("#define WASMJIT_OBJECT_DEMO_WIDGET_V1_PROP_STATUS_SET \"set_status\"")
			!= std::string::npos);
	CHECK(header.find("int32_t (*wasmjit_object_demo_widget_v1_prop_status_get_fn)"
			"(wasmjit_object_demo_widget_v1* self);") != std::string::npos);
	CHECK(header.find("void (*wasmjit_object_demo_widget_v1_prop_status_set_fn)"
			"(wasmjit_object_demo_widget_v1* self, int32_t value);")
			!= std::string::npos);
	CHECK(header.find("wasmjit_object_demo_node_v1* "
			"(*wasmjit_object_demo_widget_v1_prop_preview_get_fn)("
			"wasmjit_object_demo_widget_v1* self);") != std::string::npos);
	CHECK(header.find("{ \"status\", \"i32\", \"get_status\", \"set_status\", 1u }")
			!= std::string::npos);
	CHECK(header.find("{ \"caption\", \"i32\", \"compute_caption\", NULL, 0u }")
			!= std::string::npos);
	CHECK(header.find("{ \"preview\", \"ref defined demo.Node\", "
			"\"get_preview\", NULL, 0u }") != std::string::npos);
	// A read-only property has no setter macro at all.
	CHECK(header.find("WASMJIT_OBJECT_DEMO_WIDGET_V1_PROP_CAPTION_SET")
			== std::string::npos);

	// The handle surface (`SO-3`): the same accessors over the `i64` slot a
	// non-GC consumer carries, so one operation keeps one implementation.
	CHECK(header.find("typedef int64_t wasmjit_object_demo_widget_v1_handle;")
			!= std::string::npos);
	CHECK(header.find("#define WASMJIT_OBJECT_DEMO_WIDGET_V1_HANDLE_NONE 0")
			!= std::string::npos);
	CHECK(header.find("typedef int32_t (*wasmjit_object_demo_widget_v1_area_h_fn)"
			"(int64_t self);") != std::string::npos);
	CHECK(header.find("typedef int32_t "
			"(*wasmjit_object_demo_widget_v1_prop_status_get_h_fn)(int64_t self);")
			!= std::string::npos);
	CHECK(header.find("typedef void "
			"(*wasmjit_object_demo_widget_v1_prop_status_set_h_fn)"
			"(int64_t self, int32_t value);") != std::string::npos);
	// A reference parameter keeps its typed spelling in the handle form too.
	CHECK(header.find("(*wasmjit_object_demo_widget_v1_attach_h_fn)"
			"(int64_t self, wasmjit_object_demo_node_v1* target)")
			!= std::string::npos);

	const std::string rust = WASM::Bindings::emitRustModule(schema);
	CHECK(rust.find("pub const WASMJIT_OBJECT_DEMO_WIDGET_V1_FIELD_TARGET: u32 = 32;")
			!= std::string::npos);
	CHECK(rust.find("pub static WASMJIT_OBJECT_DEMO_WIDGET_V1: wasmjit_object_meta")
			!= std::string::npos);
	CHECK(rust.find("pub type wasmjit_object_demo_widget_v1_attach_fn = unsafe extern \"C\" fn("
			"*mut wasmjit_object_demo_widget_v1, target: *mut "
			"wasmjit_object_demo_node_v1)")
			!= std::string::npos);
	CHECK(rust.find("parent: Some(\"demo.Base\")") != std::string::npos);
	CHECK(rust.find("pub const WASMJIT_OBJECT_DEMO_WIDGET_V1_PROP_STATUS_GET: &str = "
			"\"get_status\";") != std::string::npos);
	CHECK(rust.find("pub type wasmjit_object_demo_widget_v1_prop_status_set_fn = "
			"unsafe extern \"C\" fn(*mut wasmjit_object_demo_widget_v1, i32);")
			!= std::string::npos);
	CHECK(rust.find("wasmjit_object_property_meta { name: \"status\", type_name: \"i32\", "
			"get_export: \"get_status\", set_export: Some(\"set_status\"), "
			"is_mutable: true }") != std::string::npos);
	CHECK(rust.find("properties: WASMJIT_OBJECT_DEMO_WIDGET_V1_PROPERTIES")
			!= std::string::npos);
	// An object without properties points at the empty slice, not at nothing.
	CHECK(rust.find("properties: &[],") != std::string::npos);
	CHECK(rust.find("pub type wasmjit_object_demo_widget_v1_handle = i64;")
			!= std::string::npos);
	CHECK(rust.find("pub const WASMJIT_OBJECT_DEMO_WIDGET_V1_HANDLE_NONE: i64 = 0;")
			!= std::string::npos);
	CHECK(rust.find("pub type wasmjit_object_demo_widget_v1_area_h_fn = "
			"unsafe extern \"C\" fn(i64) -> i32;") != std::string::npos);
	CHECK(rust.find("pub type wasmjit_object_demo_widget_v1_prop_status_get_h_fn = "
			"unsafe extern \"C\" fn(i64) -> i32;") != std::string::npos);

	const std::string wit = WASM::Bindings::emit(schema, WASM::Bindings::Language::Wit);
	CHECK(wit.find("interface types {") != std::string::npos);
	CHECK(wit.find("resource demo-widget-v1 {") != std::string::npos);
	CHECK(wit.find("resource demo-widget-v2 {") != std::string::npos);
	CHECK(wit.find("resource demo-node-v1 {") != std::string::npos);
	CHECK(wit.find("x: func() -> s32;") != std::string::npos);
	CHECK(wit.find("set-y: func(value: s32);") != std::string::npos);
	CHECK(wit.find("attach: func(target: borrow<demo-node-v1>);") != std::string::npos);
	CHECK(wit.find("area: func() -> s32;") != std::string::npos);
	// What WIT cannot say is documented in the emitted file, not dropped silently.
	CHECK(wit.find("label: a reference that is not another host object")
			!= std::string::npos);
	CHECK(wit.find("target: a reference field has no WIT getter") != std::string::npos);
	// A mutable reference field still gets the setter WIT can express.
	CHECK(wit.find("set-target: func(value: borrow<demo-node-v1>);") != std::string::npos);
	CHECK(wit.find("blit: `v128` parameter") != std::string::npos);
	// A packed byte is a WIT `s8`, and a packed field is one byte in the table.
	CHECK(wit.find("kind: func() -> s8;") != std::string::npos);
	// A property crosses the boundary as the call it already is: WIT has no
	// storage, so a property and a field project to the same member shape and
	// the difference is the note that says the value is computed.
	CHECK(wit.find("/// Property `status`: computed by `get_status`, not a stored slot.")
			!= std::string::npos);
	CHECK(wit.find("    status: func() -> s32;") != std::string::npos);
	CHECK(wit.find("    set-status: func(value: s32);") != std::string::npos);
	CHECK(wit.find("    caption: func() -> s32;") != std::string::npos);
	// A read-only property gets no setter, and the accessor is never emitted
	// twice: it is the property's projection, not a second member.
	CHECK(wit.find("set-caption") == std::string::npos);
	CHECK(wit.find("get-status:") == std::string::npos);
	CHECK(wit.find("preview: a property of a reference has no WIT getter")
			!= std::string::npos);

	// `SO-4`: the packaging. The host provides the objects, so the world imports
	// the interface that carries their resources; `resource.drop` is then the
	// guest-side spelling of the host's release.
	CHECK(wit.find("world demo {") != std::string::npos);
	CHECK(wit.find("// The host provides the objects") != std::string::npos);
	CHECK(wit.find("  import types;") != std::string::npos);

	// ... and the Kotlin façade holds the `i64` slot with exactly one release.
	const std::string kotlin =
			WASM::Bindings::emit(schema, WASM::Bindings::Language::Kotlin);
	INFO(kotlin);
	CHECK(kotlin.find("class DemoWidgetV1(private val handle: Long) : AutoCloseable")
			!= std::string::npos);
	CHECK(kotlin.find("class DemoWidgetV2(private val handle: Long) : AutoCloseable")
			!= std::string::npos);
	CHECK(kotlin.find("external fun wasmjit_object_demo_widget_v1_release"
			"(handle: Long)") != std::string::npos);
	CHECK(kotlin.find("if (released) return") != std::string::npos);
	CHECK(kotlin.find("const val HANDLE_NONE: Long = 0") != std::string::npos);
	CHECK(header.find("#define WASMJIT_OBJECT_DEMO_NODE_V1_SIZE 17u") != std::string::npos);

	// A C++ consumer gets an object that owns the slot: the entry points first,
	// then — *after* `extern "C"` closes, because a class cannot have C linkage —
	// one RAII class per descriptor.
	CHECK(header.find("void wasmjit_object_demo_widget_v1_release("
			"wasmjit_object_demo_widget_v1_handle self);") != std::string::npos);
	CHECK(header.find("int32_t wasmjit_object_demo_widget_v1_area("
			"wasmjit_object_demo_widget_v1_handle self);") != std::string::npos);
	const std::size_t cLinkageEnd = header.find("#ifdef __cplusplus\n}\n#endif");
	REQUIRE(cLinkageEnd != std::string::npos);
	CHECK(header.find("class DemoWidgetV1 {") > cLinkageEnd);
	CHECK(header.find("class DemoNodeV1 {") > cLinkageEnd);
	CHECK(header.find("class DemoWidgetV2 {") > cLinkageEnd);
	// Ownership: move-only, one release in the destructor, two ways to give up.
	CHECK(header.find("DemoWidgetV1(const DemoWidgetV1&) = delete;")
			!= std::string::npos);
	CHECK(header.find("DemoWidgetV1(DemoWidgetV1&& other) noexcept")
			!= std::string::npos);
	CHECK(header.find("\t\t\twasmjit_object_demo_widget_v1_release(_handle);")
			!= std::string::npos);
	// An accessor is projected as its property, so it is not a second member...
	CHECK(header.find("\tint32_t status() const { return "
			"wasmjit_object_demo_widget_v1_prop_status_get(_handle); }")
			!= std::string::npos);
	CHECK(header.find("void setStatus(int32_t value)") != std::string::npos);
	CHECK(header.find(" int32_t getStatus(") == std::string::npos);
	CHECK(header.find(" int32_t computeCaption(") == std::string::npos);
	CHECK(header.find(" setStatusValue(") == std::string::npos);
	// ... and the methods that are not accessors forward with their arguments.
	CHECK(header.find("\tint32_t area()") != std::string::npos);
	CHECK(header.find("\tvoid resize(int32_t width, int32_t height)")
			!= std::string::npos);
	CHECK(header.find("\tvoid blit(wasm_v128 lanes)") != std::string::npos);

	// A declared constructor is a C++ constructor; a parameter list that would be
	// the default (or adopting) constructor's becomes a static factory instead, so
	// one `spawn()` is still one call rather than a class that does not compile.
	CHECK(header.find("\texplicit DemoWidgetV1(int32_t width, int32_t height)\n"
			"\t\t: _handle(wasmjit_object_demo_widget_v1_create(width, height))")
			!= std::string::npos);
	CHECK(header.find("static DemoNodeV1 spawn()") != std::string::npos);
	CHECK(header.find("return DemoNodeV1(wasmjit_object_demo_node_v1_spawn());")
			!= std::string::npos);
	// A constructor whose single parameter *is* the slot type would be the
	// adopting constructor over again, so it is a factory of the same name.
	CHECK(header.find("static DemoWidgetV1 open(int64_t slot)") != std::string::npos);
	CHECK(header.find("return DemoWidgetV1(wasmjit_object_demo_widget_v1_open(slot));")
			!= std::string::npos);
	// The C side spells the same two calls: an entry point and its `_h_fn`.
	CHECK(header.find("wasmjit_object_demo_widget_v1_handle "
			"wasmjit_object_demo_widget_v1_create(int32_t width, int32_t height);")
			!= std::string::npos);
	CHECK(header.find("typedef int64_t (*wasmjit_object_demo_node_v1_spawn_h_fn)"
			"(void);") != std::string::npos);
	CHECK(rust.find("pub type wasmjit_object_demo_widget_v1_create_h_fn = "
			"unsafe extern \"C\" fn(width: i32, height: i32) -> i64;")
			!= std::string::npos);
	CHECK(wit.find("constructor(width: s32, height: s32);") != std::string::npos);
	CHECK(wit.find("constructor();") != std::string::npos);
	// WIT names every constructor `[constructor]r`, so one resource carries one:
	// the second is noted and points at the entry point that is not in WIT.
	CHECK(wit.find("a second constructor is not expressible in WIT")
			!= std::string::npos);
	CHECK(kotlin.find("fun create(width: Int, height: Int): DemoWidgetV1 =")
			!= std::string::npos);
	CHECK(kotlin.find("fun spawn(): DemoNodeV1 =") != std::string::npos);
	// Overloading is a C++ and Kotlin thing, not a WIT one.
	CHECK(kotlin.find("fun open(slot: Long): DemoWidgetV1 =") != std::string::npos);

	// A declared parameter name is the name every emitter uses, never `pN`:
	// `argNames` is the same index-aligned list a function entry carries.
	CHECK(header.find("typedef void (*wasmjit_object_demo_widget_v1_resize_fn)"
			"(wasmjit_object_demo_widget_v1* self, int32_t width, int32_t height);")
			!= std::string::npos);
	CHECK(header.find("\tvoid resize(int32_t width, int32_t height)") != std::string::npos);
	CHECK(header.find("wasmjit_object_demo_widget_v1_resize(_handle, width, height);")
			!= std::string::npos);
	CHECK(header.find("wasmjit_object_demo_widget_v1_attach(_handle, target);")
			!= std::string::npos);
	CHECK(header.find("\texplicit DemoWidgetV1(int32_t width, int32_t height)")
			!= std::string::npos);
	CHECK(wit.find("resize: func(width: s32, height: s32);") != std::string::npos);
	CHECK(wit.find("attach: func(target: borrow<demo-node-v1>);")
			!= std::string::npos);
	CHECK(wit.find("constructor(width: s32, height: s32);") != std::string::npos);
	CHECK(kotlin.find("fun create(width: Int, height: Int): DemoWidgetV1 =")
			!= std::string::npos);
	CHECK(rust.find("fn(*mut wasmjit_object_demo_widget_v1, width: i32, height: i32)")
			!= std::string::npos);
	CHECK(rust.find("_h_fn = unsafe extern \"C\" fn(width: i32, height: i32) -> i64;")
			!= std::string::npos);

	// A list shorter than the signature is legal: the rest keep their position,
	// so one name does not shift the others.
	Json partial = objectSchema();
	partial["objects"][0]["methods"][0]["argNames"] = Json::array({ "width" });
	const std::string partialHeader = WASM::Bindings::emitCHeader(
			WASM::Bindings::Schema::fromJson(partial));
	CHECK(partialHeader.find("int32_t width, int32_t p1)") != std::string::npos);

	// Without objects the emitters are byte-for-byte what they were.
	const Json plain = Json::parse(R"({
		"schema": "wasmjit.binding/1", "direction": "all", "module": "m",
		"imports": [], "exports": [] })");
	const std::string plainHeader =
			WASM::Bindings::emit(plain, WASM::Bindings::Language::CHeader);
	CHECK(plainHeader.find("wasmjit_object_meta") == std::string::npos);
	CHECK(plainHeader.find("Generated from the wasmjit.binding/1") != std::string::npos);
}

TEST_CASE("generated object bindings compile and their WIT is accepted", "[bindings][objects]")
{
	const WASM::Bindings::Schema schema = WASM::Bindings::Schema::fromJson(objectSchema());
	const std::filesystem::path dir = bindingsDir();
	const std::filesystem::path headerPath = dir / "demo_objects.h";
	const std::filesystem::path cSourcePath = dir / "demo_objects.c";
	const std::filesystem::path cLogPath = dir / "demo_objects_gcc.log";
	const std::filesystem::path rustPath = dir / "demo_objects.rs";
	const std::filesystem::path rustLogPath = dir / "demo_objects_rustc.log";
	const std::filesystem::path witPath = dir / "demo_objects.wit";
	const std::filesystem::path witJsonPath = dir / "demo_objects.wit.json";
	const std::filesystem::path witLogPath = dir / "demo_objects_wit.log";

	{
		std::ofstream header(headerPath, std::ios::binary);
		header << WASM::Bindings::emitCHeader(schema);
		// A translation unit that really uses the table: a declaration that is
		// merely included and never referenced proves too little.
		std::ofstream source(cSourcePath, std::ios::binary);
		source << "#include \"demo_objects.h\"\n"
				  "static const wasmjit_object_meta* demo_meta(void)\n"
				  "{\n"
				  "    return &wasmjit_object_demo_widget_v1_meta;\n"
				  "}\n"
				  "static int32_t demo_status(wasmjit_object_demo_widget_v1* self)\n"
				  "{\n"
				  "    wasmjit_object_demo_widget_v1_prop_status_get_fn get = 0;\n"
				  "    (void)get;\n"
				  "    return (int32_t)(self != 0);\n"
				  "}\n"
				  "static wasmjit_object_demo_widget_v1_handle demo_handle(void)\n"
				  "{\n"
				  "    wasmjit_object_demo_widget_v1_area_h_fn area = 0;\n"
				  "    (void)area;\n"
				  "    return WASMJIT_OBJECT_DEMO_WIDGET_V1_HANDLE_NONE;\n"
				  "}\n"
				  "int main(void)\n"
				  "{\n"
				  "    return (int)(demo_meta()->size\n"
				  "            + WASMJIT_OBJECT_DEMO_WIDGET_V1_FIELD_TARGET\n"
				  "            + WASMJIT_OBJECT_DEMO_WIDGET_V1_SIZE\n"
				  "            + demo_meta()->property_count\n"
				  "            + (unsigned)demo_handle()\n"
				  "            + (unsigned)sizeof(WASMJIT_OBJECT_DEMO_WIDGET_V1_PROP_STATUS_GET));\n"
				  "}\n";
		std::ofstream rust(rustPath, std::ios::binary);
		rust << WASM::Bindings::emitRustModule(schema);
		std::ofstream wit(witPath, std::ios::binary);
		wit << WASM::Bindings::emit(schema, WASM::Bindings::Language::Wit);
	}

	if (haveTool("gcc --version > /dev/null 2>&1")) {
		const std::string command = "gcc -std=c11 -fsyntax-only -I\"" + dir.string()
				+ "\" \"" + cSourcePath.string() + "\" > \"" + cLogPath.string()
				+ "\" 2>&1";
		INFO(readText(cLogPath));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("gcc not found; skipped the generated object header compile check");
	}

	// The C++ half is not just syntax: the wrapper owns the slot, so the test
	// counts releases. Each `return N` is one broken ownership rule; the entry
	// points are stubs, which is exactly what an embedder writes.
	if (haveTool("g++ --version > /dev/null 2>&1")) {
		const std::filesystem::path cppPath = dir / "demo_objects_wrapper.cpp";
		const std::filesystem::path binary = dir / "demo_objects_wrapper";
		const std::filesystem::path cppLogPath = dir / "demo_objects_gxx.log";
		{
			std::ofstream source(cppPath, std::ios::binary);
			source << R"cpp(#include "demo_objects.h"
#include <cstdint>
#include <type_traits>
#include <utility>

// A wrapper owns one slot: copy is gone, move is not.
static_assert(!std::is_copy_constructible<DemoWidgetV1>::value,
              "a wrapper owns a slot: copy is disabled");
static_assert(!std::is_copy_assignable<DemoWidgetV1>::value,
              "a wrapper owns a slot: copy is disabled");
static_assert(std::is_move_constructible<DemoWidgetV1>::value,
              "a wrapper owns a slot: move is allowed");
// Every descriptor gets a class, methods or not.
static_assert(std::is_move_constructible<DemoNodeV1>::value, "node wrapper");
static_assert(std::is_move_constructible<DemoWidgetV2>::value, "v2 wrapper");

extern "C" {

int releases = 0;
int64_t lastSlot = 0;

void wasmjit_object_demo_widget_v1_release(wasmjit_object_demo_widget_v1_handle self)
{
    ++releases;
    lastSlot = self;
}
int32_t wasmjit_object_demo_widget_v1_area(wasmjit_object_demo_widget_v1_handle self)
{
    return static_cast<int32_t>(self & 0xffff);
}
int32_t wasmjit_object_demo_widget_v1_prop_status_get(
        wasmjit_object_demo_widget_v1_handle self)
{
    (void)self;
    return 7;
}
void wasmjit_object_demo_widget_v1_prop_status_set(
        wasmjit_object_demo_widget_v1_handle self, int32_t value)
{
    (void)self;
    lastSlot = value;
}
int nodeReleases = 0;
wasmjit_object_demo_widget_v1_handle wasmjit_object_demo_widget_v1_create(
        int32_t p0, int32_t p1)
{
    return (static_cast<wasmjit_object_demo_widget_v1_handle>(p0) << 32)
         | static_cast<wasmjit_object_demo_widget_v1_handle>(
                 static_cast<uint32_t>(p1));
}
wasmjit_object_demo_node_v1_handle wasmjit_object_demo_node_v1_spawn(void)
{
    return 0x300000001LL;
}
wasmjit_object_demo_widget_v1_handle wasmjit_object_demo_widget_v1_open(int64_t p0)
{
    return p0;
}
void wasmjit_object_demo_node_v1_release(wasmjit_object_demo_node_v1_handle self)
{
    (void)self;
    ++nodeReleases;
}

}

int main()
{
    const wasmjit_object_demo_widget_v1_handle slot = 0x100000005LL;
    {
        DemoWidgetV1 widget(slot);
        if (!widget) return 1;
        if (widget.area() != 5) return 2;
        if (widget.status() != 7) return 3;
        widget.setStatus(9);
        if (lastSlot != 9) return 4;
        DemoWidgetV1 moved(std::move(widget));
        if (widget.valid()) return 5;
        if (releases != 0) return 6;
        DemoWidgetV1 assigned;
        assigned = std::move(moved);
        if (moved.valid() || !assigned.valid()) return 7;
        if (releases != 0) return 8;
    }
    if (releases != 1) return 9;
    if (lastSlot != slot) return 10;
    {
        // `reset()` twice releases once; `release()` gives the slot up silently.
        DemoWidgetV1 a(42), b(43);
        a.reset();
        a.reset();
        if (releases != 2) return 11;
        const wasmjit_object_demo_widget_v1_handle gave = b.release();
        if (gave != 43 || b.valid()) return 12;
    }
    if (releases != 2) return 13;
    {
        // Assigning over a live slot releases the old one, exactly once.
        DemoWidgetV1 c(50), d(51);
        c = std::move(d);
        if (releases != 3) return 14;
    }
    if (releases != 4) return 15;
    {
        // A declared constructor is a constructor; an empty parameter list becomes
        // a static factory, because it would be the default constructor.
        DemoWidgetV1 made(4, 6);
        if (!made) return 16;
        if (made.handle() != ((static_cast<wasmjit_object_demo_widget_v1_handle>(4)
                               << 32) | 6LL)) return 17;
        DemoNodeV1 node = DemoNodeV1::spawn();
        if (!node) return 18;
        if (nodeReleases != 0) return 19;
        // The slot-typed parameter cannot be a second constructor, so `open` is a
        // factory — and the slot it returns is adopted, not cloned.
        DemoWidgetV1 opened = DemoWidgetV1::open(0x400000002LL);
        if (!opened) return 22;
        if (opened.handle() != 0x400000002LL) return 23;
        if (releases != 4) return 24;
    }
    // `made`, `opened` released their slots at the end of the block; the factory's
    // wrapper released the node's slot exactly once.
    if (releases != 6) return 20;
    if (nodeReleases != 1) return 21;
    return 0;
}
)cpp";
		}
		const std::string compile = "g++ -std=c++17 -Wall -Wextra -I\"" + dir.string()
				+ "\" \"" + cppPath.string() + "\" -o \"" + binary.string()
				+ "\" > \"" + cppLogPath.string() + "\" 2>&1";
		INFO(readText(cppLogPath));
		REQUIRE(std::system(compile.c_str()) == 0);
		const std::string run = "\"" + binary.string() + "\" > \""
				+ cppLogPath.string() + "\" 2>&1";
		INFO(readText(cppLogPath));
		REQUIRE(std::system(run.c_str()) == 0);
	} else {
		WARN("g++ not found; skipped the generated C++ wrapper check");
	}

	if (haveTool("rustc --version > /dev/null 2>&1")) {
		const std::filesystem::path rustLib = dir / "demo_objects.rlib";
		const std::string command = "rustc --crate-type lib --edition 2021 \""
				+ rustPath.string() + "\" -o \"" + rustLib.string() + "\" > \""
				+ rustLogPath.string() + "\" 2>&1";
		INFO(readText(rustLogPath));
		REQUIRE(std::system(command.c_str()) == 0);
	} else {
		WARN("rustc not found; skipped the generated object Rust compile check");
	}

	if (!haveTool("command -v wasm-tools > /dev/null 2>&1")) {
		WARN("wasm-tools not found; skipped the object WIT parse check");
		return;
	}
	const std::string command = "wasm-tools component wit '" + witPath.string()
			+ "' --json > '" + witJsonPath.string() + "' 2> '"
			+ witLogPath.string() + "'";
	INFO(readText(witLogPath));
	REQUIRE(std::system(command.c_str()) == 0);
}
