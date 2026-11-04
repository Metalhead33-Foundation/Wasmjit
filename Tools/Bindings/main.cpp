#include "BindingSchema.hpp"

#include <Euphemy/Config/GlobalConfig.hpp>
#include <Euphemy/Io/EuphFile.hpp>

#include "WasmBase/WasmStore.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

// The vendored Euphemy library expects the host executable to define its
// configuration object; the test harness and consumer satisfy the same contract.
Euph::Conf::Configuration GLOBAL_CONFIGURATION;

namespace {

void usage(std::ostream& out)
{
	out << "usage: wasmjit-bindings <module.wasm> [options]\n"
		   "  --direction g2h|h2g|g2g   adapter direction (default: g2h)\n"
		   "  --emit schema|c-header|rust|typescript|assemblyscript|zig|kotlin|\n"
		   "         moonbit|go|dart (default: schema)\n"
		   "  --module-name NAME       name in the schema envelope\n"
		   "  --output FILE            write to FILE instead of stdout\n";
}

bool parseDirection(std::string_view text, WASM::Bindings::Direction& direction)
{
	if (text == "g2h") {
		direction = WASM::Bindings::Direction::GuestToHost;
		return true;
	}
	if (text == "h2g") {
		direction = WASM::Bindings::Direction::HostToGuest;
		return true;
	}
	if (text == "g2g") {
		direction = WASM::Bindings::Direction::GuestToGuest;
		return true;
	}
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	std::string input;
	std::string moduleName;
	std::string outputPath;
	std::string emit = "schema";
	WASM::Bindings::Direction direction = WASM::Bindings::Direction::GuestToHost;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			usage(std::cout);
			return 0;
		}
		if (arg == "--direction" && i + 1 < argc) {
			if (!parseDirection(argv[++i], direction)) {
				std::cerr << "unknown direction: " << argv[i] << "\n";
				return 2;
			}
		} else if (arg == "--emit" && i + 1 < argc) {
			emit = argv[++i];
		} else if (arg == "--module-name" && i + 1 < argc) {
			moduleName = argv[++i];
		} else if (arg == "--output" && i + 1 < argc) {
			outputPath = argv[++i];
		} else if (!arg.empty() && arg.front() == '-') {
			std::cerr << "unknown option: " << arg << "\n";
			return 2;
		} else if (input.empty()) {
			input = arg;
		} else {
			std::cerr << "unexpected argument: " << arg << "\n";
			return 2;
		}
	}

	if (input.empty()) {
		usage(std::cerr);
		return 2;
	}
	const bool wantSchema = emit == "schema";
	WASM::Bindings::Language language = WASM::Bindings::Language::CHeader;
	if (!wantSchema && !WASM::Bindings::parseLanguage(emit, language)) {
		std::cerr << "unknown --emit value: " << emit << "\n";
		return 2;
	}
	if (moduleName.empty())
		moduleName = std::filesystem::path(input).stem().string();

	std::string text;
	try {
		WASM::Store store;
		WASM::Module module;
		Euph::Io::File file(input.c_str(), Elv::Io::Mode::READ);
		module.fromFile(file, store);
		const nlohmann::ordered_json schema = WASM::Bindings::moduleSchema(
				module, store.types(), direction, moduleName);
		text = wantSchema ? schema.dump(2) + "\n"
						  : WASM::Bindings::emit(schema, language);
	} catch (const std::exception& error) {
		std::cerr << "wasmjit-bindings: " << error.what() << "\n";
		return 1;
	}

	if (outputPath.empty()) {
		std::cout << text;
		return 0;
	}
	std::ofstream out(outputPath, std::ios::binary);
	if (!out) {
		std::cerr << "wasmjit-bindings: cannot open '" << outputPath << "'\n";
		return 1;
	}
	out << text;
	return out ? 0 : 1;
}
