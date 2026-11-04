#include "BindingSchema.hpp"
#include "frontend/cpp/CppFrontend.hpp"

#include <Euphemy/Config/GlobalConfig.hpp>
#include <Euphemy/Io/EuphFile.hpp>

#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmStore.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

// The vendored Euphemy library expects the host executable to define its
// configuration object; the test harness and consumer satisfy the same contract.
Euph::Conf::Configuration GLOBAL_CONFIGURATION;

namespace {

void usage(std::ostream& out)
{
	out << "usage: wasmjit-bindings [module.wasm] [options]\n"
		   "  --direction g2h|h2g|g2g   adapter direction (default: g2h)\n"
		   "  --emit schema|c-header|rust|typescript|assemblyscript|zig|kotlin|\n"
		   "         moonbit|go|dart|java|csharp|fsharp|python|ruby|lua|swift|\n"
		   "         d|nim|crystal|ada|pascal|haskell|cpp-adapter|cpp-typedefs|\n"
		   "         cpp-source (default: schema)\n"
		   "  --module-name NAME       name in the schema envelope\n"
		   "  --candidates HEADER      SWIG-parse a C++ header and validate its\n"
		   "                           candidate signatures against the schema\n"
		   "  --glue-candidates SRC    compile a Glue registration source and\n"
		   "                           validate its printed declarations\n"
		   "  --frontend-schema        with --candidates/--glue-candidates, emit a\n"
		   "                           schema for functions taking VMContext* first\n"
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
	std::string candidatesPath;
	std::string glueCandidatesPath;
	bool frontendSchema = false;
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
		} else if (arg == "--candidates" && i + 1 < argc) {
			candidatesPath = argv[++i];
		} else if (arg == "--glue-candidates" && i + 1 < argc) {
			glueCandidatesPath = argv[++i];
		} else if (arg == "--frontend-schema") {
			frontendSchema = true;
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

	if (input.empty() && !frontendSchema) {
		usage(std::cerr);
		return 2;
	}
	if (!candidatesPath.empty() && !glueCandidatesPath.empty()) {
		std::cerr << "wasmjit-bindings: choose --candidates or "
					 "--glue-candidates, not both\n";
		return 2;
	}
	if (frontendSchema && candidatesPath.empty() && glueCandidatesPath.empty()) {
		std::cerr << "wasmjit-bindings: --frontend-schema needs --candidates or "
					 "--glue-candidates\n";
		return 2;
	}
	const bool wantSchema = emit == "schema";
	WASM::Bindings::Language language = WASM::Bindings::Language::CHeader;
	if (!wantSchema && !WASM::Bindings::parseLanguage(emit, language)) {
		std::cerr << "unknown --emit value: " << emit << "\n";
		return 2;
	}
	if (moduleName.empty()) {
		const std::string source = !candidatesPath.empty() ? candidatesPath
				: (!glueCandidatesPath.empty() ? glueCandidatesPath : input);
		moduleName = std::filesystem::path(source).stem().string();
	}

	std::string text;
	try {
		std::string error;
		const bool glue = !glueCandidatesPath.empty();
		const auto loadCandidates = [&]() {
			return glue
					? WASM::Bindings::glueCandidates(glueCandidatesPath, error)
					: WASM::Bindings::swigCandidates(candidatesPath, error);
		};
		if (frontendSchema) {
			const auto candidates = loadCandidates();
			if (!error.empty()) {
				std::cerr << "wasmjit-bindings: " << error << "\n";
				return 1;
			}
			text = WASM::Bindings::candidateSchema(candidates,
					WASM::Bindings::directionName(direction), moduleName,
					glue ? "glue" : "swig").dump(2) + "\n";
		} else {
			WASM::Store store;
			Euph::Io::File file(input.c_str(), Elv::Io::Mode::READ);
			const std::unique_ptr<WASM::Artifact> artifact =
					WASM::Artifact::parse(file, store);

			nlohmann::ordered_json schema;
			if (artifact->isCoreModule()) {
				schema = WASM::Bindings::moduleSchema(*artifact->asCoreModule(),
						store.types(), direction, moduleName);
			} else {
				WASM::Component* component = artifact->asComponent();
				component->validate();
				schema = WASM::Bindings::componentSchema(*component, store.types(),
						direction, moduleName);
			}
			if (!candidatesPath.empty() || !glueCandidatesPath.empty()) {
				const auto candidates = loadCandidates();
				if (!error.empty()) {
					std::cerr << "wasmjit-bindings: " << error << "\n";
					return 1;
				}
				text = WASM::Bindings::crossCheckCandidates(schema, candidates,
						glue ? "glue" : "swig").dump(2) + "\n";
			} else {
				text = wantSchema ? schema.dump(2) + "\n"
						  : WASM::Bindings::emit(schema, language);
			}
		}
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
