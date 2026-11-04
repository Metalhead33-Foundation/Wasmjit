#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <Euphemy/Io/EuphFile.hpp>

#include "BindingSchema.hpp"
#include "helper.hpp"
#include "TestStore.hpp"
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

void loadTestComponent(const char* name, WASM::Component& component)
{
	const std::string path = std::string(WASM_TEST_DIR) + name + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	component.fromFile(file, testStore());
	component.validate();
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
			WASM::Bindings::Direction::GuestToHost, "asmjit_i32_basic");

	REQUIRE(schema.at("schema") == "wasmjit.binding/1");
	REQUIRE(schema.at("direction") == "g2h");
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
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "wasm_js_string_length");

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

	const std::string header = WASM::Bindings::emitCHeader(schema);
	REQUIRE(header.find("typedef struct wasm_externref wasm_externref;") != std::string::npos);
	REQUIRE(header.find("wasm_externref* arg0 /* borrowed */") != std::string::npos);
	REQUIRE(header.find("/* ownership: explicit */") != std::string::npos);
	REQUIRE(header.find("typeId") == std::string::npos);
	REQUIRE(header.find("sizeof(") == std::string::npos);

	const std::string rust = WASM::Bindings::emitRustModule(schema);
	REQUIRE(rust.find("pub struct wasm_externref") != std::string::npos);
	REQUIRE(rust.find("pub type wasmjit_identity_t") != std::string::npos);
	REQUIRE(rust.find("pub fn wasmjit_wasm_js_string_length") != std::string::npos);
	REQUIRE(rust.find("extern \"C\"") != std::string::npos);
}

TEST_CASE("defined references use a kind name, not a numeric TypeId, in the header", "[bindings]")
{
	const WASM::Module module = loadTestModule("plain_ref");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "plain_ref");

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
			WASM::Bindings::Direction::GuestToHost, "exact_ref");

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
			WASM::Bindings::Direction::GuestToHost, "wasm_js_string_length");
	const std::filesystem::path dir = bindingsDir();

	const std::filesystem::path headerPath = dir / "wasm_js_string_length.h";
	const std::filesystem::path cSourcePath = dir / "wasm_js_string_length.c";
	const std::filesystem::path rustPath = dir / "wasm_js_string_length.rs";
	const std::filesystem::path cLogPath = dir / "gcc.log";
	const std::filesystem::path rustLogPath = dir / "rustc.log";

	{
		std::ofstream header(headerPath, std::ios::binary);
		header << WASM::Bindings::emitCHeader(schema);
		std::ofstream source(cSourcePath, std::ios::binary);
		source << "#include \"wasm_js_string_length.h\"\nint main(void) { return 0; }\n";
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
	// because the fixture uses tail calls, bulk memory and reference types;
	// wasm-opt 124 was the host version when this check was added (2026-11-04).
	const std::filesystem::path input =
			std::filesystem::path(WASM_TEST_DIR) / "asmjit_i32_basic.wasm";
	const std::filesystem::path output = bindingsDir() / "asmjit_i32_basic.opt.wasm";
	const std::filesystem::path log = bindingsDir() / "wasm-opt.log";
	const std::string command = "wasm-opt --all-features -O2 \"" + input.string()
			+ "\" -o \"" + output.string() + "\" > \"" + log.string() + "\" 2>&1";
	INFO(readText(log));
	REQUIRE(std::system(command.c_str()) == 0);

	const Json baseline = WASM::Bindings::moduleSchema(loadTestModule("asmjit_i32_basic"),
			testStore().types(), WASM::Bindings::Direction::GuestToHost,
			"asmjit_i32_basic");

	WASM::Store optimizedStore;
	WASM::Module optimizedModule;
	Euph::Io::File file(output.c_str(), Elv::Io::Mode::READ);
	optimizedModule.fromFile(file, optimizedStore);
	const Json optimized = WASM::Bindings::moduleSchema(optimizedModule,
			optimizedStore.types(), WASM::Bindings::Direction::GuestToHost,
			"asmjit_i32_basic");

	for (const Json& original : baseline.at("exports")) {
		if (original.value("kind", "") != "function")
			continue;
		const std::string name = original.at("name").get<std::string>();
		const Json* match = findExport(optimized, name);
		REQUIRE(match != nullptr);
		REQUIRE(match->at("kind") == original.at("kind"));
		REQUIRE(match->at("type").at("params") == original.at("type").at("params"));
		REQUIRE(match->at("type").at("results") == original.at("type").at("results"));
	}
}


TEST_CASE("the remaining language emitters produce declarations", "[bindings]")
{
	const WASM::Module module = loadTestModule("wasm_js_string_length");
	const Json schema = WASM::Bindings::moduleSchema(module, testStore().types(),
			WASM::Bindings::Direction::HostToGuest, "wasm_js_string_length");
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

	const std::string ts = WASM::Bindings::emit(schema,
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
	REQUIRE(zig.find("pub extern \"wasm:js-string\" fn") != std::string::npos);
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

	const std::string go = WASM::Bindings::emit(schema, WASM::Bindings::Language::Go);
	REQUIRE(go.find("//go:wasmimport wasm:js-string length") != std::string::npos);
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
	const Json schema = WASM::Bindings::componentSchema(component, testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "component_runtime_import");

	REQUIRE(schema.at("kind") == "component");
	const Json* import = findByName(schema.at("imports"), "host-add");
	REQUIRE(import != nullptr);
	REQUIRE(import->at("kind") == "function");
	REQUIRE(import->at("type").at("kind") == "func");
	REQUIRE(import->at("type").at("params").size() == 2u);
	REQUIRE(import->at("type").at("params")[0].at("type") == "u32");
	REQUIRE(import->at("type").at("params")[0].at("name") == "a");
	REQUIRE(import->at("type").at("results")[0].at("type") == "u32");

	const Json* exportEntry = findByName(schema.at("exports"), "host-add");
	REQUIRE(exportEntry != nullptr);
	REQUIRE(exportEntry->at("kind") == "function");
	REQUIRE(exportEntry->at("type").at("kind") == "func");
	REQUIRE(exportEntry->at("type").at("params").size() == 2u);
	REQUIRE(exportEntry->at("type").at("results")[0].at("type") == "u32");
}

TEST_CASE("the binding schema projects component records and type imports", "[bindings]")
{
	WASM::Component component;
	loadTestComponent("component_typeimports", component);
	const Json schema = WASM::Bindings::componentSchema(component, testStore().types(),
			WASM::Bindings::Direction::GuestToHost, "component_typeimports");

	const Json* import = findByName(schema.at("imports"), "r");
	REQUIRE(import != nullptr);
	REQUIRE(import->at("kind") == "type");

	const Json* exportEntry = findByName(schema.at("exports"), "t");
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
			WASM::Bindings::Direction::GuestToHost, "component_runtime_import");
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
	REQUIRE(c.find("uint32_t wasmjit_host_add") != std::string::npos);
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

	const std::string zig = WASM::Bindings::emit(schema, WASM::Bindings::Language::Zig);
	REQUIRE(zig.find("pub extern \"component\" fn") != std::string::npos);
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
	REQUIRE(go.find("//go:wasmexport host-add") != std::string::npos);
	REQUIRE(go.find("//go:wasmimport component host-add") != std::string::npos);
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

	const std::string kotlin = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::Kotlin);
	REQUIRE(kotlin.find("external fun wasmjit_component_host_add") != std::string::npos);
	const std::string moonbit = WASM::Bindings::emit(schema,
			WASM::Bindings::Language::MoonBit);
	REQUIRE(moonbit.find("extern \"wasm\" fn wasmjit_component_host_add")
			!= std::string::npos);
	const std::string dart = WASM::Bindings::emit(schema, WASM::Bindings::Language::Dart);
	REQUIRE(dart.find("@Native<Uint32 Function(Uint32, Uint32)>") != std::string::npos);
}

