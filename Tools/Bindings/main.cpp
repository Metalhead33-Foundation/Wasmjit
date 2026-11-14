#include "BindingSchema.hpp"
#include "frontend/cpp/CppFrontend.hpp"
#include "frontend/wit/WitFrontend.hpp"

#include <Euphemy/Io/EuphFile.hpp>

#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmStore.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string rejectedWitFeature(const WASM::Bindings::Schema& schema,
		bool rejectAsync, bool rejectUnstable)
{
	const auto check = [&](const std::vector<WASM::Bindings::Entry>& entries)
			-> std::string {
		for (const WASM::Bindings::Entry& entry : entries) {
			if (rejectAsync && entry.raw.value("witAsync", false))
				return "async WIT function '" + entry.name
						+ "' rejected by --wit-reject-async";
			if (rejectUnstable && entry.raw.contains("stability")
					&& entry.raw.at("stability").is_object()
					&& entry.raw.at("stability").contains("unstable"))
				return "unstable WIT function '" + entry.name
						+ "' rejected by --wit-reject-unstable";
		}
		return {};
	};
	std::string error = check(schema.imports);
	if (error.empty())
		error = check(schema.exports);
	return error;
}

void usage(std::ostream& out)
{
	out << "usage: wasmjit-bindings [module.wasm|schema.json] [options]\n"
		   "  --schema FILE            import a complete JSON schema instead of a\n"
		   "                           module/component\n"
		   "  --direction g2h|h2g|g2g|all\n"
		   "                           adapter direction (default: g2h)\n"
		   "  --emit schema|c-header|rust|typescript|assemblyscript|zig|kotlin|\n"
		   "         moonbit|go|dart|java|csharp|fsharp|python|ruby|lua|swift|\n"
		   "         d|nim|crystal|ada|pascal|haskell|cpp-adapter|cpp-typedefs|\n"
		   "         cpp-source|wit|witx (default: schema)\n"
		   "  --module-name NAME       name in the schema envelope\n"
		   "  --candidates HEADER      SWIG-parse a C++ header and validate its\n"
		   "                           candidate signatures against the schema\n"
		   "  --glue-candidates SRC    compile a Glue registration source and\n"
		   "                           validate its printed declarations\n"
		   "  --wit PATH               WIT text/package/directory input (invokes\n"
		   "                           wasm-tools component wit --json)\n"
		   "  --wit-json FILE          WIT JSON input produced by wasm-tools\n"
		   "  --witx FILE              legacy WITX input (Preview 1 semantic names)\n"
		   "  --wit-world NAME         select a WIT world (default: first)\n"
		   "  --wit-package NAME       select the package that owns the world\n"
		   "  --wit-reject-async       reject async WIT functions instead of\n"
		   "                           emitting documented synchronous stubs\n"
		   "                           (default: allow synchronous stubs)\n"
		   "  --wit-reject-unstable    reject WIT entries marked @unstable\n"
		   "  --frontend-schema        with --candidates/--glue-candidates, derive a\n"
		   "                           G2H imports schema (or emit any --emit target)\n"
		   "  --output FILE            write to FILE instead of stdout\n";
}

} // namespace

int main(int argc, char** argv)
{
	std::string input;
	std::string moduleName;
	std::string outputPath;
	std::string candidatesPath;
	std::string glueCandidatesPath;
	std::string witPath;
	std::string witJsonPath;
	std::string witWorld;
	std::string witxPath;
	std::string witPackage;
	bool frontendSchema = false;
	bool schemaInput = false;
	bool rejectAsync = false;
	bool allowAsync = false;
	bool rejectUnstable = false;
	std::string emit = "schema";
	WASM::Bindings::Direction direction = WASM::Bindings::Direction::GuestToHost;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			usage(std::cout);
			return 0;
		}
		if (arg == "--direction" && i + 1 < argc) {
			if (!WASM::Bindings::parseDirection(argv[++i], direction)) {
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
		} else if (arg == "--wit" && i + 1 < argc) {
			witPath = argv[++i];
		} else if (arg == "--wit-json" && i + 1 < argc) {
			witJsonPath = argv[++i];
		} else if (arg == "--witx" && i + 1 < argc) {
			witxPath = argv[++i];
		} else if (arg == "--wit-world" && i + 1 < argc) {
			witWorld = argv[++i];
		} else if (arg == "--wit-package" && i + 1 < argc) {
			witPackage = argv[++i];
		} else if (arg == "--wit-reject-async") {
			rejectAsync = true;
		} else if (arg == "--wit-allow-async") {
			allowAsync = true;
		} else if (arg == "--wit-reject-unstable") {
			rejectUnstable = true;
		} else if (arg == "--schema" && i + 1 < argc) {
			input = argv[++i];
			schemaInput = true;
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

	if (!schemaInput && !input.empty()
			&& std::filesystem::path(input).extension() == ".json")
		schemaInput = true;
	const bool witInput = !witPath.empty() || !witJsonPath.empty();
	const bool witxInput = !witxPath.empty();
	if (!witPath.empty() && !witJsonPath.empty()) {
		std::cerr << "wasmjit-bindings: choose --wit or --wit-json, not both\n";
		return 2;
	}
	if ((rejectAsync || allowAsync || rejectUnstable || !witPackage.empty())
			&& !witInput) {
		std::cerr << "wasmjit-bindings: WIT policy/selection flags "
				 "require WIT input\n";
		return 2;
	}
	if (rejectAsync && allowAsync) {
		std::cerr << "wasmjit-bindings: choose --wit-allow-async or "
				 "--wit-reject-async, not both\n";
		return 2;
	}
	if (witInput && (!input.empty() || schemaInput || !candidatesPath.empty()
			|| !glueCandidatesPath.empty() || frontendSchema)) {
		std::cerr << "wasmjit-bindings: WIT input cannot be combined with a "
					 "module, schema or C++ candidate input\n";
		return 2;
	}
	if (witxInput && (witInput || !input.empty() || schemaInput
			|| !candidatesPath.empty() || !glueCandidatesPath.empty()
			|| frontendSchema)) {
		std::cerr << "wasmjit-bindings: --witx cannot be combined with WIT, "
				 "module, schema or C++ candidate input\n";
		return 2;
	}
	if (input.empty() && !witInput && !witxInput && !frontendSchema) {
		usage(std::cerr);
		return 2;
	}
	if (schemaInput && (!candidatesPath.empty() || !glueCandidatesPath.empty()
			|| frontendSchema || witInput)) {
		std::cerr << "wasmjit-bindings: --schema cannot be combined with "
					 "--candidates, --glue-candidates, --frontend-schema or WIT\n";
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
	if (moduleName.empty() && !schemaInput) {
		const std::string source = !witPath.empty() ? witPath
				: (!witJsonPath.empty() ? witJsonPath
				: (!candidatesPath.empty() ? candidatesPath
				: (!glueCandidatesPath.empty() ? glueCandidatesPath : input)));
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
		if (witxInput) {
			const WASM::Bindings::Schema schema =
					WASM::Bindings::witxSchemaFromFile(witxPath, direction,
							moduleName, error);
			if (!error.empty()) {
				std::cerr << "wasmjit-bindings: " << error << "\n";
				return 1;
			}
			text = wantSchema ? schema.toJson().dump(2) + "\n"
					: WASM::Bindings::emit(schema, language);
		} else if (witInput) {
			const std::filesystem::path path = !witPath.empty() ? witPath : witJsonPath;
			WASM::Bindings::Schema schema = WASM::Bindings::witSchemaFromFile(
					path, direction, moduleName, witWorld, !witJsonPath.empty(), error,
					rejectAsync ? WASM::Bindings::WitAsyncPolicy::Reject
							: WASM::Bindings::WitAsyncPolicy::AllowSynchronous,
					witPackage);
			if (!error.empty()) {
				std::cerr << "wasmjit-bindings: " << error << "\n";
				return 1;
			}
			if (rejectAsync || rejectUnstable) {
				const std::string rejected =
						rejectedWitFeature(schema, rejectAsync, rejectUnstable);
				if (!rejected.empty()) {
					std::cerr << "wasmjit-bindings: " << rejected << "\n";
					return 1;
				}
			}
			text = wantSchema ? schema.toJson().dump(2) + "\n"
						  : WASM::Bindings::emit(schema, language);
		} else if (schemaInput) {
			std::ifstream stream(input, std::ios::binary);
			if (!stream)
				throw std::runtime_error("cannot open schema '" + input + "'");
			nlohmann::ordered_json imported;
			stream >> imported;
			WASM::Bindings::Schema schema =
					WASM::Bindings::projectSchema(
							WASM::Bindings::Schema::fromJson(imported), direction);
			if (!moduleName.empty()) {
				schema.module = moduleName;
				if (direction == WASM::Bindings::Direction::GuestToGuest) {
					for (WASM::Bindings::Entry& entry : schema.imports)
						entry.module = moduleName;
				}
			}
			text = wantSchema ? schema.toJson().dump(2) + "\n"
							  : WASM::Bindings::emit(schema, language);
		} else if (frontendSchema) {
			const auto candidates = loadCandidates();
			if (!error.empty()) {
				std::cerr << "wasmjit-bindings: " << error << "\n";
				return 1;
			}
			const WASM::Bindings::Schema schema =
					WASM::Bindings::candidateSchemaModel(candidates, direction,
							moduleName, glue ? "glue" : "swig");
			text = wantSchema ? schema.toJson().dump(2) + "\n"
						  : WASM::Bindings::emit(schema, language);
		} else {
			WASM::Store store;
			Euph::Io::File file(input.c_str(), Elv::Io::Mode::READ);
			const std::unique_ptr<WASM::Artifact> artifact =
					WASM::Artifact::parse(file, store);

			WASM::Bindings::Schema schema;
			if (artifact->isCoreModule()) {
				schema = WASM::Bindings::moduleSchemaModel(*artifact->asCoreModule(),
						store.types(), direction, moduleName);
			} else {
				WASM::Component* component = artifact->asComponent();
				component->validate();
				schema = WASM::Bindings::componentSchemaModel(*component, store.types(),
						direction, moduleName);
			}
			if (!candidatesPath.empty() || !glueCandidatesPath.empty()) {
				const auto candidates = loadCandidates();
				if (!error.empty()) {
					std::cerr << "wasmjit-bindings: " << error << "\n";
					return 1;
				}
				text = WASM::Bindings::crossCheckCandidates(schema.toJson(), candidates,
						glue ? "glue" : "swig").dump(2) + "\n";
			} else {
				text = wantSchema ? schema.toJson().dump(2) + "\n"
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
