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

// docs/EMBEDDING_SURFACE.md §12.8: conformance tests for the first-party
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
	const std::string compile = "g++ -std=c++2a -I\"" + dir.string() + "\" -I\""
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
#include <Euphemy/Config/GlobalConfig.hpp>

Euph::Conf::Configuration GLOBAL_CONFIGURATION;
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
#include <Euphemy/Config/GlobalConfig.hpp>
#include <cstdint>

Euph::Conf::Configuration GLOBAL_CONFIGURATION;
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
#include <Euphemy/Config/GlobalConfig.hpp>
#include <cstdint>

Euph::Conf::Configuration GLOBAL_CONFIGURATION;
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
#include <Euphemy/Config/GlobalConfig.hpp>

Euph::Conf::Configuration GLOBAL_CONFIGURATION;
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
	REQUIRE(schema.at("exports").size() == 1u);
	const Json& exportEntry = schema.at("exports")[0];
	REQUIRE(exportEntry.at("name") == "host_add");
	REQUIRE(exportEntry.at("type").at("params").size() == 2u);
	REQUIRE(exportEntry.at("type").at("params")[0].at("type") == "i32");
	REQUIRE(exportEntry.at("type").at("results")[0].at("type") == "i32");

	if (std::filesystem::is_regular_file(
				"/usr/local/lib/Glue-1.5/libGlue.a")) {
		const std::filesystem::path glueSource = dir / "host_glue.cpp";
		{
			std::ofstream out(glueSource, std::ios::binary);
			out << "#include <glue/value.h>\n"
				   "#include <cstdint>\n"
				   "namespace WASM { struct VMContext; }\n"
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
		REQUIRE(glueSchema.at("exports").size() == 1u);
		REQUIRE(glueSchema.at("exports")[0].at("name") == "host_add");
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
	REQUIRE(header.find("namespace WASM { struct VMContext; struct ModuleInstance; }")
			!= std::string::npos);

	const std::string source = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::CppSource);
	REQUIRE(source.find("wasmjit_add_fn wasmjit_add = nullptr;")
			!= std::string::npos);
	REQUIRE(source.find("wasmjit_asmjit_i32_basic_load_callables("
			"WASM::ModuleInstance* instance)") != std::string::npos);
	REQUIRE(source.find("reinterpret_cast<wasmjit_add_fn>(callable->fnPtr)")
			!= std::string::npos);

	const Json g2h = WASM::Bindings::moduleSchema(
			loadTestModule("wasm_js_string_length"), testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "wasm_js_string_length");
	const std::string importHeader = WASM::Bindings::emit(g2h,
			WASM::Bindings::Language::CppTypedefs);
	REQUIRE(importHeader.find("cpp-typedefs is an H2G") != std::string::npos);
	const std::string importSource = WASM::Bindings::emit(g2h,
			WASM::Bindings::Language::CppSource);
	REQUIRE(importSource.find("cpp-source is an H2G") != std::string::npos);

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
#include <Euphemy/Config/GlobalConfig.hpp>

Euph::Conf::Configuration GLOBAL_CONFIGURATION;

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
	const std::string compile = "g++ -std=c++2a -I\"" + dir.string() + "\" -I\""
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
}

