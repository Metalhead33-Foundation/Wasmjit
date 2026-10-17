#include "WastScript.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <Euphemy/Io/EuphFile.hpp>
#include <jit/jit.h>
#include <nlohmann/json.hpp>

#include "LibJit/LibjitModuleCompiler.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmStore.hpp"
#include "WasmBase/WasmTrap.hpp"
#include "WasmBase/WasmType.hpp"

namespace Spec {

namespace {

using Json = nlohmann::json;

// ── JSON access helpers ─────────────────────────────────────────────────────

std::string stringOr(const Json& object, const char* key)
{
	if (!object.is_object())
		return std::string();
	const auto it = object.find(key);
	if (it == object.end() || !it->is_string())
		return std::string();
	return it->get<std::string>();
}

int intOr(const Json& object, const char* key)
{
	if (!object.is_object())
		return 0;
	const auto it = object.find(key);
	if (it == object.end() || !it->is_number())
		return 0;
	return it->get<int>();
}

// ── Model construction ──────────────────────────────────────────────────────

ScriptValue valueFromJson(const Json& json)
{
	ScriptValue value;
	value.type = stringOr(json, "type");
	value.laneType = stringOr(json, "lane_type");

	if (json.is_object()) {
		const auto it = json.find("value");
		if (it != json.end()) {
			if (it->is_string()) {
				value.hasValue = true;
				value.value = it->get<std::string>();
			} else if (it->is_array()) {
				value.hasValue = true;
				for (const Json& lane : *it) {
					if (lane.is_string())
						value.lanes.push_back(lane.get<std::string>());
					else if (lane.is_number())
						value.lanes.push_back(std::to_string(lane.get<long long>()));
				}
			}
		}
	}
	return value;
}

ScriptAction actionFromJson(const Json& json)
{
	ScriptAction action;
	action.type = stringOr(json, "type");
	action.module = stringOr(json, "module");
	action.field = stringOr(json, "field");

	if (json.is_object()) {
		const auto it = json.find("args");
		if (it != json.end() && it->is_array()) {
			for (const Json& arg : *it)
				action.args.push_back(valueFromJson(arg));
		}
	}
	return action;
}

ScriptCommand commandFromJson(const Json& json)
{
	ScriptCommand command;
	command.type = stringOr(json, "type");
	command.line = intOr(json, "line");
	command.name = stringOr(json, "name");
	command.filename = stringOr(json, "filename");
	command.moduleType = stringOr(json, "module_type");
	command.text = stringOr(json, "text");
	command.as = stringOr(json, "as");

	if (json.is_object()) {
		const auto actionIt = json.find("action");
		if (actionIt != json.end())
			command.action = actionFromJson(*actionIt);

		const auto expectedIt = json.find("expected");
		if (expectedIt != json.end() && expectedIt->is_array()) {
			for (const Json& expected : *expectedIt)
				command.expected.push_back(valueFromJson(expected));
		}
	}
	return command;
}

// ── Type helpers ────────────────────────────────────────────────────────────

bool typeCodeFromName(std::string_view name, WASM::ValueTypeCode& out)
{
	if (name == "i32") { out = WASM::ValueTypeCode::I32; return true; }
	if (name == "i64") { out = WASM::ValueTypeCode::I64; return true; }
	if (name == "f32") { out = WASM::ValueTypeCode::F32; return true; }
	if (name == "f64") { out = WASM::ValueTypeCode::F64; return true; }
	if (name == "v128") { out = WASM::ValueTypeCode::V128; return true; }
	return false;
}

const char* typeName(WASM::ValueTypeCode code)
{
	switch (code) {
	case WASM::ValueTypeCode::I32: return "i32";
	case WASM::ValueTypeCode::I64: return "i64";
	case WASM::ValueTypeCode::F32: return "f32";
	case WASM::ValueTypeCode::F64: return "f64";
	case WASM::ValueTypeCode::V128: return "v128";
	default: return "unsupported";
	}
}

// We can only marshal scalar values with a literal representation; reference
// and vector values (and "any function reference" placeholders) are skipped.
// Marshal a reference argument (docs/TESTING.md, GC.md R5). The engine has no
// host-object model for references yet (GC.md R4), so this is an *identity*
// mapping, chosen to be stable so `ref.eq` holds across invocations:
//   * `(ref.null …)`              → null;
//   * `(ref.extern N)`/`(ref.host N)` → a non-null synthetic pointer derived
//     from N (`ref.test externref` is true for it, which is all these tests
//     need — an unknown pointer classifies as `extern`);
//   * `(ref.i31 K)`               → the tagged immediate the module itself
//     would build, `(K << 1) | 1`;
//   * `(ref.func N)`              → the module's own callable for function
//     index N, via the same sentinel resolution the runtime uses.
void* referenceArgument(WASM::ModuleInstance& instance, WASM::ValueTypeCode code, const ScriptValue& arg)
{
	if (!arg.hasValue || arg.value.empty())
		return nullptr;
	if (arg.value == "null")
		return nullptr;

	const long long raw = std::strtoll(arg.value.c_str(), nullptr, 10);
	switch (code) {
	case WASM::ValueTypeCode::FuncRef: {
		WASM::Value sentinel;
		sentinel.kind = WASM::ValueTypeCode::FuncRef;
		sentinel.ref = reinterpret_cast<void*>(static_cast<uintptr_t>(raw));
		return instance.callableFromSegmentRef(sentinel);
	}
	case WASM::ValueTypeCode::I31Ref:
		return reinterpret_cast<void*>(static_cast<uintptr_t>((raw << 1) | 1));
	default:
		// Host-owned: an even, non-null, never-GC-heap pointer.
		return reinterpret_cast<void*>(static_cast<uintptr_t>(0xE0000000ull + 16ull * static_cast<unsigned long long>(raw)));
	}
}

// Reference type names wasm-tools emits for *arguments* (GC.md R5). Deliberately
// separate from `typeCodeFromName`, which also gates the expected-*result* path:
// a reference result generally has no canonical pointer to compare against
// (`(ref.null any)`), so results stay skipped while arguments become runnable.
bool referenceArgTypeCode(const ScriptValue& value, WASM::ValueTypeCode& out)
{
	if (!value.hasValue)
		return false;
	if (value.type == "funcref") { out = WASM::ValueTypeCode::FuncRef; return true; }
	if (value.type == "externref") { out = WASM::ValueTypeCode::ExternRef; return true; }
	if (value.type == "anyref") { out = WASM::ValueTypeCode::AnyRef; return true; }
	if (value.type == "eqref") { out = WASM::ValueTypeCode::EqRef; return true; }
	if (value.type == "i31ref") { out = WASM::ValueTypeCode::I31Ref; return true; }
	if (value.type == "structref") { out = WASM::ValueTypeCode::StructRef; return true; }
	if (value.type == "arrayref") { out = WASM::ValueTypeCode::ArrayRef; return true; }
	return false;
}

bool isReferenceArg(const ScriptValue& value)
{
	WASM::ValueTypeCode code = WASM::ValueTypeCode::Void;
	return referenceArgTypeCode(value, code);
}

// True for the reference types `referenceArgument` can marshal.
bool isMarshallableReference(WASM::ValueTypeCode code)
{
	switch (code) {
	case WASM::ValueTypeCode::FuncRef:
	case WASM::ValueTypeCode::ExternRef:
	case WASM::ValueTypeCode::AnyRef:
	case WASM::ValueTypeCode::EqRef:
	case WASM::ValueTypeCode::I31Ref:
	case WASM::ValueTypeCode::StructRef:
	case WASM::ValueTypeCode::ArrayRef:
	case WASM::ValueTypeCode::NullRef:
	case WASM::ValueTypeCode::NullFuncRef:
	case WASM::ValueTypeCode::NullExternRef:
		return true;
	default:
		return false;
	}
}

bool isScalarValue(const ScriptValue& value)
{
	if (!value.hasValue)
		return false;
	WASM::ValueTypeCode code = WASM::ValueTypeCode::Void;
	return typeCodeFromName(value.type, code);
}

// ── v128 marshalling (proposals/SIMD.md S7) ────────────────────────────────
//
// A `v128` crosses the harness boundary as the same 16 bytes the engine uses, so
// the script's per-lane strings are decoded lane by lane in little-endian order.
// Integer lanes are signed decimals; float lanes carry the raw IEEE-754 bit
// pattern, exactly like the scalar encodings above.

// One float lane against its script text: a bit pattern, or one of the two NaN
// policies the spec allows (`got` is already widened to 64 bits).
bool floatLaneMatches(const std::string& want, uint64_t got,
		uint64_t absMask, uint64_t canonical, uint64_t arithmeticMask)
{
	if (want == "nan:canonical")
		return (got & absMask) == canonical;
	if (want == "nan:arithmetic")
		return (got & arithmeticMask) == arithmeticMask;
	return got == std::strtoull(want.c_str(), nullptr, 10);
}

// The lane type names the *lane*, not the shape: `wasm-tools` emits "i8" with 16
// lanes, "i16" with 8, "i32"/"f32" with 4 and "i64"/"f64" with 2. The width comes
// from the name and the count has to agree with it (lanes * width == 16).
bool laneWidth(std::string_view laneType, std::size_t& width, bool& isFloat)
{
	if (laneType == "i8") { width = 1; isFloat = false; return true; }
	if (laneType == "i16") { width = 2; isFloat = false; return true; }
	if (laneType == "i32") { width = 4; isFloat = false; return true; }
	if (laneType == "i64") { width = 8; isFloat = false; return true; }
	if (laneType == "f32") { width = 4; isFloat = true; return true; }
	if (laneType == "f64") { width = 8; isFloat = true; return true; }
	return false;
}

bool decodeV128(std::string_view laneType, const std::vector<std::string>& lanes, unsigned char* out)
{
	std::size_t width = 0;
	bool isFloat = false;
	if (!laneWidth(laneType, width, isFloat) || lanes.size() * width != 16)
		return false;

	for (std::size_t i = 0; i < lanes.size(); ++i) {
		// Integer lanes are signed decimals; float lanes are the raw bit pattern.
		// Either way only the low `width` bytes are kept, which is the lane's
		// little-endian encoding.
		const uint64_t bits = isFloat
				? std::strtoull(lanes[i].c_str(), nullptr, 10)
				: static_cast<uint64_t>(std::strtoll(lanes[i].c_str(), nullptr, 10));
		std::memcpy(out + i * width, &bits, width);
	}
	return true;
}

// Lane-wise comparison. The float NaN policies apply per lane, because a vector
// operation's NaN result is exactly as unspecified as a scalar one's — which is
// why `f32x4.min` can pass an `assert_return` with `nan:canonical` in a lane.
bool compareV128(const ScriptValue& expected, const unsigned char* data)
{
	std::size_t width = 0;
	bool isFloat = false;
	if (!laneWidth(expected.laneType, width, isFloat)
			|| expected.lanes.size() * width != 16)
		return false;

	const uint64_t mask = width == 8 ? ~uint64_t{0} : ((uint64_t{1} << (width * 8)) - 1);
	for (std::size_t i = 0; i < expected.lanes.size(); ++i) {
		uint64_t got = 0;
		std::memcpy(&got, data + i * width, width);
		const std::string& want = expected.lanes[i];

		if (isFloat) {
			const bool ok = width == 4
					? floatLaneMatches(want, got, 0x7FFFFFFFull, 0x7FC00000ull, 0x7FC00000ull)
					: floatLaneMatches(want, got, 0x7FFFFFFFFFFFFFFFull,
							0x7FF8000000000000ull, 0x7FF8000000000000ull);
			if (!ok)
				return false;
			continue;
		}

		// Signed decimal, masked to the lane's width so `-1` is `0xFF` in an i8 lane.
		const uint64_t value = static_cast<uint64_t>(std::strtoll(want.c_str(), nullptr, 10));
		if ((got & mask) != (value & mask))
			return false;
	}
	return true;
}

// Formats a returned scalar for failure messages. Without this a mismatch only
// reports the *type* of the unexpected value, which makes diagnosing a spec
// failure needlessly hard.
std::string formatResultValue(WASM::ValueTypeCode code, const unsigned char* data)
{
	std::ostringstream out;
	switch (code) {
	case WASM::ValueTypeCode::I32: {
		int32_t v = 0;
		std::memcpy(&v, data, sizeof(v));
		out << v;
		break;
	}
	case WASM::ValueTypeCode::I64: {
		int64_t v = 0;
		std::memcpy(&v, data, sizeof(v));
		out << v;
		break;
	}
	case WASM::ValueTypeCode::F32: {
		uint32_t bits = 0;
		float v = 0.0f;
		std::memcpy(&bits, data, sizeof(bits));
		std::memcpy(&v, data, sizeof(v));
		out << v << " (0x" << std::hex << bits << ")";
		break;
	}
	case WASM::ValueTypeCode::F64: {
		uint64_t bits = 0;
		double v = 0.0;
		std::memcpy(&bits, data, sizeof(bits));
		std::memcpy(&v, data, sizeof(v));
		out << v << " (0x" << std::hex << bits << ")";
		break;
	}
	case WASM::ValueTypeCode::V128: {
		// No lane type is available here (the declared result type is just
		// `v128`), so the dump is the 16 bytes as hex, most significant first —
		// enough to see which lane differs.
		out << "0x" << std::hex;
		for (int i = 15; i >= 0; --i) {
			const unsigned byte = data[i];
			if (byte < 16)
				out << '0';
			out << byte;
		}
		break;
	}
	default:
		out << "?";
		break;
	}
	return out.str();
}

// ── The `spectest` host module ──────────────────────────────────────────────

// The spec scripts import a handful of helpers from a host module called
// `spectest`. The runtime currently resolves *function* imports only, so that
// is all we expose here; scripts that import its memory/table/globals are
// reported as failures rather than silently misbehaving.
void spectestPrint(WASM::VMContext*) {}
void spectestPrintI32(WASM::VMContext*, int32_t) {}
void spectestPrintI64(WASM::VMContext*, int64_t) {}
void spectestPrintF32(WASM::VMContext*, float) {}
void spectestPrintF64(WASM::VMContext*, double) {}
void spectestPrintI32F32(WASM::VMContext*, int32_t, float) {}
void spectestPrintF64F64(WASM::VMContext*, double, double) {}

// ── Script execution ────────────────────────────────────────────────────────

class ScriptRunner
{
public:
	ScriptRunner(std::string wasmDir, LibJIT::Context& jitContext)
		: wasmDir(std::move(wasmDir)), jitContext(jitContext)
	{
		installSpectest();
	}

	ScriptReport run(const Script& script);

private:
	// Backing storage for one dynamically-marshalled argument. 16 bytes is
	// exactly one `v128`, and covers every scalar too; `alignas(16)` matches the
	// ABI's requirement for a 16-byte aggregate (SIMD-0).
	struct ArgSlot {
		alignas(16) unsigned char raw[16] = {};
	};

	std::string wasmDir;
	LibJIT::Context& jitContext;

	// Imports visible to every module in this script: the `spectest` stubs plus
	// whatever earlier `register` commands exposed.
	WASM::RegistryImportResolver registry;

	// A ModuleInstance holds a raw pointer to its Module, so both containers
	// must keep their contents alive (and stable) for the whole script.
	std::vector<std::unique_ptr<WASM::Module>> modules;
	std::vector<std::unique_ptr<WASM::ModuleInstance>> instances;

	std::unordered_map<std::string, WASM::ModuleInstance*> namedInstances;
	WASM::ModuleInstance* lastInstance = nullptr;

	ScriptReport report;

	// Set by invoke() when the call trapped (a non-local exit out of the JIT).
	const char* lastTrapMessage = nullptr;
	// The kind of that trap, which is what assert_trap is checked against
	// (docs/SAFETY.md §2.4): the free-text message is only a hint.
	WASM::TrapKind lastTrapKind = WASM::TrapKind::Internal;

	void installSpectest();

	void pass();
	void fail(int line, const std::string& message);
	void skip(const std::string& reason);

	WASM::ModuleInstance* instanceFor(const std::string& name, int line);
	const WASM::FuncType* funcTypeFor(const WASM::ModuleInstance& instance, const WASM::Callable& callable);

	void runCommand(const ScriptCommand& command);
	void onModule(const ScriptCommand& command);
	void onRegister(const ScriptCommand& command);
	void onAssertReturn(const ScriptCommand& command);
	void onAssertTrap(const ScriptCommand& command);
	void onAction(const ScriptCommand& command);
	void onAssertExhaustion(const ScriptCommand& command);

	bool invoke(WASM::ModuleInstance& instance, const ScriptAction& action, const WASM::FuncType& funcType,
				const WASM::Callable& callable, std::array<unsigned char, 16>& returns, bool& trapped);
	bool compareResult(const ScriptValue& expected, WASM::ValueTypeCode actual, const unsigned char* data);
};

} // namespace

Script Script::parse(std::string_view json)
{
	const Json document = Json::parse(json);

	Script script;
	script.sourceFilename = stringOr(document, "source_filename");

	if (document.is_object()) {
		const auto it = document.find("commands");
		if (it != document.end() && it->is_array()) {
			for (const Json& command : *it)
				script.commands.push_back(commandFromJson(command));
		}
	}
	return script;
}

ScriptReport runScript(const Script& script, const std::string& wasmDir, LibJIT::Context& jitContext)
{
	ScriptRunner runner(wasmDir, jitContext);
	return runner.run(script);
}

void ScriptRunner::installSpectest()
{
	const auto register_ = [this](const char* name, void* fn) {
		registry.registerFunction("spectest", name, WASM::Callable{fn, nullptr, 0});
	};

	register_("print", reinterpret_cast<void*>(&spectestPrint));
	register_("print_i32", reinterpret_cast<void*>(&spectestPrintI32));
	register_("print_i64", reinterpret_cast<void*>(&spectestPrintI64));
	register_("print_f32", reinterpret_cast<void*>(&spectestPrintF32));
	register_("print_f64", reinterpret_cast<void*>(&spectestPrintF64));
	register_("print_i32_f32", reinterpret_cast<void*>(&spectestPrintI32F32));
	register_("print_f64_f64", reinterpret_cast<void*>(&spectestPrintF64F64));

	// The spec's `spectest` host module also exports a table and a memory, and
	// test scripts import them. Provide the same shapes the reference interpreter
	// uses (table 10..20 funcref, memory 1..2 pages). Both are owned by the
	// global Store and created once for the whole process.
	WASM::Store& store = WASM::Store::global();

	static WASM::TableInstance* spectestTable = nullptr;
	if (spectestTable == nullptr)
		spectestTable = store.table(store.createTable(10, 20));
	registry.registerTable("spectest", "table", WASM::ImportedTable{spectestTable});

	static WASM::LinearMemory* spectestMemory = nullptr;
	if (spectestMemory == nullptr)
		spectestMemory = store.memory(store.createLinearMemory(1, 2, false));
	registry.registerMemory("spectest", "memory", spectestMemory);

	// The 64-bit-index variants. Import matching does not compare the index
	// type yet (docs/proposals/GC.md R1), so the name is what makes a
	// `spectest.table64` import resolve; `table64.wast` needs it to
	// instantiate at all.
	static WASM::TableInstance* spectestTable64 = nullptr;
	if (spectestTable64 == nullptr)
		spectestTable64 = store.table(store.createTable(10, 20));
	registry.registerTable("spectest", "table64", WASM::ImportedTable{spectestTable64});

	static WASM::LinearMemory* spectestMemory64 = nullptr;
	if (spectestMemory64 == nullptr)
		spectestMemory64 = store.memory(store.createLinearMemory(1, 2, false));
	registry.registerMemory("spectest", "memory64", spectestMemory64);

	// The scalar globals the reference interpreter's `spectest` exports. The
	// engine now resolves imported globals (docs/proposals/MUTABLE_GLOBALS.md
	// MG-1), so scripts that read them (extended-const offsets, imported
	// funcref globals) need the real values.
	registry.registerGlobal("spectest", "global_i32", WASM::Value{.i32 = 666, .kind = WASM::ValueTypeCode::I32});
	registry.registerGlobal("spectest", "global_i64", WASM::Value{.i64 = 666, .kind = WASM::ValueTypeCode::I64});
	registry.registerGlobal("spectest", "global_f32", WASM::Value{.f32 = 666.6f, .kind = WASM::ValueTypeCode::F32});
	registry.registerGlobal("spectest", "global_f64", WASM::Value{.f64 = 666.6, .kind = WASM::ValueTypeCode::F64});
}

void ScriptRunner::pass()
{
	++report.passed;
}

void ScriptRunner::fail(int line, const std::string& message)
{
	++report.failed;
	report.failures.push_back("line " + std::to_string(line) + ": " + message);
}

void ScriptRunner::skip(const std::string& reason)
{
	++report.skipped;
	report.skipReasons.push_back(reason);
}

WASM::ModuleInstance* ScriptRunner::instanceFor(const std::string& name, int line)
{
	if (!name.empty()) {
		const auto it = namedInstances.find(name);
		if (it == namedInstances.end()) {
			fail(line, "action refers to unknown module '" + name + "'");
			return nullptr;
		}
		return it->second;
	}

	if (lastInstance == nullptr) {
		fail(line, "no module has been instantiated yet");
		return nullptr;
	}
	return lastInstance;
}

const WASM::FuncType* ScriptRunner::funcTypeFor(const WASM::ModuleInstance& instance, const WASM::Callable& callable)
{
	const WASM::Module* module = instance.context()->module;
	if (module == nullptr || callable.localTypeIdx >= module->types.size())
		return nullptr;

	const WASM::Subtype& subtype = module->types[callable.localTypeIdx];
	if (!subtype.isFunction())
		return nullptr;

	return &std::get<WASM::FuncType>(subtype.composite);
}

ScriptReport ScriptRunner::run(const Script& script)
{
	for (const ScriptCommand& command : script.commands)
		runCommand(command);
	return report;
}

void ScriptRunner::runCommand(const ScriptCommand& command)
{
	// WJ_TRACE=1 prints every command before it runs. Because each script runs
	// in a forked child whose report is only written at the end, this is the
	// way to find *which* command killed a child ("produced no report").
	if (std::getenv("WJ_TRACE") != nullptr) {
		std::fprintf(stderr, "[wj] line %d: %s %s\n", command.line, command.type.c_str(),
					 command.action.field.c_str());
		std::fflush(stderr);
	}

	if (command.type == "module") {
		onModule(command);
		return;
	}
	if (command.type == "register") {
		onRegister(command);
		return;
	}
	if (command.type == "assert_return") {
		onAssertReturn(command);
		return;
	}
	if (command.type == "action") {
		onAction(command);
		return;
	}

	// The remaining command families need runtime features that do not exist
	// yet. They are reported as skipped so the suite stays actionable while the
	// JIT grows instead of drowning real regressions in expected failures.
	if (command.type == "assert_exhaustion") {
		onAssertExhaustion(command);
		return;
	}
	if (command.type == "assert_trap") {
		onAssertTrap(command);
		return;
	}
	if (command.type == "assert_malformed" || command.type == "assert_invalid"
			|| command.type == "assert_unlinkable" || command.type == "assert_uninstantiable") {
		skip("module validation: malformed/invalid modules are not rejected yet");
		return;
	}
	if (command.type == "module_definition" || command.type == "module_instance") {
		skip("module-linking proposal");
		return;
	}
	if (command.type == "assert_exception") {
		skip("exception-handling proposal");
		return;
	}

	skip("unrecognised command type '" + command.type + "'");
}

void ScriptRunner::onModule(const ScriptCommand& command)
{
	if (command.moduleType != "binary") {
		skip("text-format module '" + command.filename + "' (only binary modules can be loaded)");
		return;
	}

	std::string path = wasmDir;
	if (!path.empty() && path.back() != '/')
		path.push_back('/');
	path += command.filename;

	if (std::getenv("WJ_TRACE") != nullptr) {
		std::fprintf(stderr, "[wj]   parsing %s\n", command.filename.c_str());
		std::fflush(stderr);
	}

	auto module = std::make_unique<WASM::Module>();
	try {
		Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
		module->fromFile(file);
	} catch (const std::exception& error) {
		fail(command.line, "could not load '" + command.filename + "': " + error.what());
		return;
	}

	modules.push_back(std::move(module));

	if (std::getenv("WJ_TRACE") != nullptr) {
		std::fprintf(stderr, "[wj]   instantiating %s\n", command.filename.c_str());
		std::fflush(stderr);
	}

	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
	std::unique_ptr<WASM::ModuleInstance> instance;
	try {
		instance = compiler.instantiate(*modules.back(), registry);
	} catch (const std::exception& error) {
		fail(command.line, "instantiation of '" + command.filename + "' failed: " + error.what());
		return;
	} catch (...) {
		fail(command.line, "instantiation of '" + command.filename + "' failed");
		return;
	}

	if (instance == nullptr) {
		fail(command.line, "instantiation of '" + command.filename + "' produced no instance");
		return;
	}

	WASM::ModuleInstance* raw = instance.get();
	instances.push_back(std::move(instance));
	lastInstance = raw;

	if (!command.name.empty())
		namedInstances[command.name] = raw;

	pass();
}

void ScriptRunner::onRegister(const ScriptCommand& command)
{
	WASM::ModuleInstance* instance = instanceFor(command.name, command.line);
	if (instance == nullptr)
		return;

	instance->registerExports(registry, command.as);
	pass();
}

void ScriptRunner::onAssertReturn(const ScriptCommand& command)
{
	const ScriptAction& action = command.action;

	if (action.type != "invoke") {
		skip("assert_return with a non-invoke action ('" + action.type + "')");
		return;
	}

	WASM::ModuleInstance* instance = instanceFor(action.module, command.line);
	if (instance == nullptr)
		return;

	const auto callable = instance->exportedFunction(action.field);
	if (!callable.has_value()) {
		fail(command.line, "no exported function named '" + action.field + "'");
		return;
	}

	const WASM::FuncType* funcType = funcTypeFor(*instance, *callable);
	if (funcType == nullptr) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}

	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
		return;
	}
	if (funcType->results.size() > 1) {
		skip("multiple return values are not marshalled yet");
		return;
	}
	if (command.expected.size() != funcType->results.size()) {
		fail(command.line, "result count mismatch for '" + action.field + "'");
		return;
	}

	for (const ScriptValue& arg : action.args) {
		if (!isScalarValue(arg) && !isReferenceArg(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}
	for (const ScriptValue& expected : command.expected) {
		if (!isScalarValue(expected)) {
			skip("expected type '" + expected.type + "' is not supported");
			return;
		}
	}

	std::array<unsigned char, 16> returns {};
	bool trapped = false;
	if (!invoke(*instance, action, *funcType, *callable, returns, trapped)) {
		skip("could not marshal the arguments of '" + action.field + "'");
		return;
	}
	if (trapped) {
		fail(command.line, "unexpected trap from '" + action.field + "': "
				+ std::string(lastTrapMessage != nullptr ? lastTrapMessage : "trap"));
		return;
	}

	if (command.expected.empty()) {
		pass();
		return;
	}

	const WASM::ValueTypeCode resultCode = funcType->results[0].val.opcode;
	if (compareResult(command.expected[0], resultCode, returns.data())) {
		pass();
		return;
	}

	std::stringstream message;
	message << "assert_return failed for '" << action.field << "': expected "
			<< command.expected[0].type << " " << command.expected[0].value
			<< " but got " << typeName(resultCode) << " "
			<< formatResultValue(resultCode, returns.data());
	fail(command.line, message.str());
}

void ScriptRunner::onAssertTrap(const ScriptCommand& command)
{
	// Runs the action in-process: the engine's traps are recoverable now
	// (docs/proposals/BULK_MEMORY.md BM-2), so a trap means the scope armed by
	// invoke() jumped back; no trap means the module did not trap.
	const ScriptAction& action = command.action;

	if (action.type != "invoke") {
		skip("assert_trap with a non-invoke action ('" + action.type + "')");
		return;
	}

	WASM::ModuleInstance* instance = instanceFor(action.module, command.line);
	if (instance == nullptr)
		return;

	const auto callable = instance->exportedFunction(action.field);
	if (!callable.has_value()) {
		fail(command.line, "no exported function named '" + action.field + "'");
		return;
	}

	const WASM::FuncType* funcType = funcTypeFor(*instance, *callable);
	if (funcType == nullptr) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}
	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
		return;
	}
	if (funcType->results.size() > 1) {
		skip("multiple return values are not marshalled yet");
		return;
	}
	for (const ScriptValue& arg : action.args) {
		if (!isScalarValue(arg) && !isReferenceArg(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}

	std::array<unsigned char, 16> returns {};
	bool trapped = false;
	if (!invoke(*instance, action, *funcType, *callable, returns, trapped)) {
		skip("could not marshal the arguments of '" + action.field + "'");
		return;
	}

	if (trapped) {
		// Check *why* it trapped, not just that it did (docs/SAFETY.md §2.4):
		// the expectation is compared against the trap's kind, so a trap raised
		// for the wrong reason can no longer satisfy assert_trap. The engine's
		// free-text message is reported on a mismatch to help diagnose it.
		const std::string got = WASM::trapKindName(lastTrapKind);
		if (command.text.empty() || command.text == got) {
			pass();
			return;
		}
		fail(command.line, "expected trap '" + command.text + "' but trapped with '" + got +
			"' (engine message: '" + (lastTrapMessage != nullptr ? lastTrapMessage : "?") + "')");
		return;
	}
	fail(command.line, "expected a trap from '" + action.field + "' but it returned normally");
}

void ScriptRunner::onAction(const ScriptCommand& command)
{
	const ScriptAction& action = command.action;

	if (action.type != "invoke") {
		skip("action of type '" + action.type + "' is not supported");
		return;
	}

	WASM::ModuleInstance* instance = instanceFor(action.module, command.line);
	if (instance == nullptr)
		return;

	const auto callable = instance->exportedFunction(action.field);
	if (!callable.has_value()) {
		fail(command.line, "no exported function named '" + action.field + "'");
		return;
	}

	const WASM::FuncType* funcType = funcTypeFor(*instance, *callable);
	if (funcType == nullptr) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}

	if (funcType->params.size() != action.args.size() || funcType->results.size() > 1) {
		skip("action '" + action.field + "' cannot be marshalled yet");
		return;
	}

	for (const ScriptValue& arg : action.args) {
		if (!isScalarValue(arg) && !isReferenceArg(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}

	std::array<unsigned char, 16> returns {};
	bool trapped = false;
	if (!invoke(*instance, action, *funcType, *callable, returns, trapped)) {
		skip("could not marshal the arguments of '" + action.field + "'");
		return;
	}
	if (trapped) {
		fail(command.line, "unexpected trap from '" + action.field + "': "
				+ std::string(lastTrapMessage != nullptr ? lastTrapMessage : "trap"));
		return;
	}

	pass();
}

void ScriptRunner::onAssertExhaustion(const ScriptCommand& command)
{
	// Approximated: the engine reports stack exhaustion by terminating
	// abnormally (a real stack overflow, or a trap via std::abort()), so run
	// the action in a *nested* child and treat an abnormal end as the expected
	// exhaustion. A normal return is a failure — the call was supposed to
	// exhaust the stack. `assert_trap` (whose recovery the runtime does not
	// model yet) stays skipped.
	const ScriptAction& action = command.action;

	if (action.type != "invoke") {
		skip("assert_exhaustion with a non-invoke action ('" + action.type + "')");
		return;
	}

	WASM::ModuleInstance* instance = instanceFor(action.module, command.line);
	if (instance == nullptr)
		return;

	const auto callable = instance->exportedFunction(action.field);
	if (!callable.has_value()) {
		fail(command.line, "no exported function named '" + action.field + "'");
		return;
	}

	const WASM::FuncType* funcType = funcTypeFor(*instance, *callable);
	if (funcType == nullptr) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}
	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
		return;
	}
	if (funcType->results.size() > 1) {
		skip("multiple return values are not marshalled yet");
		return;
	}
	for (const ScriptValue& arg : action.args) {
		if (!isScalarValue(arg) && !isReferenceArg(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}

	const pid_t child = ::fork();
	if (child < 0) {
		skip("could not fork to observe stack exhaustion");
		return;
	}
	if (child == 0) {
		std::signal(SIGABRT, SIG_DFL);
		std::signal(SIGSEGV, SIG_DFL);
		std::signal(SIGBUS, SIG_DFL);
		std::signal(SIGFPE, SIG_DFL);
		std::array<unsigned char, 16> returns {};
		bool trapped = false;
		invoke(*instance, action, *funcType, *callable, returns, trapped);
		_exit(0); // returned normally: no exhaustion
	}

	int status = 0;
	while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	const bool abnormal = WIFSIGNALED(status) ||
		(WIFEXITED(status) && WEXITSTATUS(status) != 0);
	if (abnormal) {
		pass();
		return;
	}
	fail(command.line, "assert_exhaustion: '" + action.field +
		"' returned instead of exhausting the stack");
}

bool ScriptRunner::invoke(WASM::ModuleInstance& instance, const ScriptAction& action,
						  const WASM::FuncType& funcType, const WASM::Callable& callable,
						  std::array<unsigned char, 16>& returns, bool& trapped)
{
	std::vector<ArgSlot> slots(funcType.params.size());
	std::vector<void*> arguments(funcType.params.size() + 1);

	// Slot 0 is always the implicit VMContext pointer that the JIT ABI expects
	// as the first argument of every compiled function.
	void* context = instance.context();
	arguments[0] = &context;

	for (std::size_t i = 0; i < funcType.params.size(); ++i) {
		const ScriptValue& arg = action.args[i];

		WASM::ValueTypeCode code = WASM::ValueTypeCode::Void;
		const bool refArg = referenceArgTypeCode(arg, code);
		if (!refArg) {
			if (!typeCodeFromName(arg.type, code) || code != funcType.params[i].val.opcode)
				return false;
		} else if (!WASM::isReferenceType(funcType.params[i].val.opcode)) {
			// A reference argument for a scalar parameter is a marshalling bug,
			// not a test to skip.
			return false;
		}

		switch (code) {
		case WASM::ValueTypeCode::I32:
			*reinterpret_cast<int32_t*>(slots[i].raw) =
					static_cast<int32_t>(std::strtoll(arg.value.c_str(), nullptr, 10));
			break;
		case WASM::ValueTypeCode::I64:
			*reinterpret_cast<int64_t*>(slots[i].raw) =
					static_cast<int64_t>(std::strtoll(arg.value.c_str(), nullptr, 10));
			break;
		case WASM::ValueTypeCode::F32: {
			// Float literals carry the raw IEEE-754 bit pattern.
			const uint32_t bits = static_cast<uint32_t>(std::strtoull(arg.value.c_str(), nullptr, 10));
			std::memcpy(slots[i].raw, &bits, sizeof(bits));
			break;
		}
		case WASM::ValueTypeCode::F64: {
			const uint64_t bits = std::strtoull(arg.value.c_str(), nullptr, 10);
			std::memcpy(slots[i].raw, &bits, sizeof(bits));
			break;
		}
		case WASM::ValueTypeCode::V128:
			// The lanes are decoded into the slot the JIT's ABI expects: the same
			// 16 bytes the engine stores a `v128` in (proposals/SIMD.md S7).
			if (!decodeV128(arg.laneType, arg.lanes, slots[i].raw))
				return false;
			break;
		default:
			// Reference arguments (GC.md R5). Without this a reference-taking
			// action is skipped, which silently leaves fixtures like
			// `ref_test.wast`'s `(invoke "init" (ref.extern 0))` un-run and makes
			// every dependent `ref.test` look wrong.
			if (!isMarshallableReference(code))
				return false;
			*reinterpret_cast<void**>(slots[i].raw) = referenceArgument(instance, code, arg);
			break;
		}

		arguments[i + 1] = slots[i].raw;
	}

	// jit_apply knows the calling convention for an arbitrary signature, so we
	// can marshal runtime-typed arguments without generating a template
	// instantiation for every (arity, type) combination. The signature is
	// rebuilt per invocation (libjit keeps its own copy), which is fine for a
	// test harness but would be worth caching in a hot path.
	const jit_type_t signature = jitContext.getTranslator().translateFunctionSignature(funcType);

	// Arm a trap scope: a Wasm trap longjmps back to here instead of aborting
	// the process (docs/proposals/BULK_MEMORY.md BM-2), so the harness can
	// report it — and assert_trap can expect it.
	WASM::TrapScope trapScope;
	if (setjmp(trapScope.buffer) != 0) {
		trapped = true;
		lastTrapMessage = trapScope.message();
		lastTrapKind = trapScope.kind();
		return true;
	}

	jit_apply(signature, callable.fnPtr, arguments.data(),
			  static_cast<unsigned int>(funcType.params.size() + 1), returns.data());
	return true;
}

bool ScriptRunner::compareResult(const ScriptValue& expected, WASM::ValueTypeCode actual, const unsigned char* data)
{
	WASM::ValueTypeCode expectedCode = WASM::ValueTypeCode::Void;
	if (!expected.hasValue || !typeCodeFromName(expected.type, expectedCode) || expectedCode != actual)
		return false;

	switch (actual) {
	case WASM::ValueTypeCode::I32: {
		int32_t got = 0;
		std::memcpy(&got, data, sizeof(got));
		const int64_t want = std::strtoll(expected.value.c_str(), nullptr, 10);
		return got == static_cast<int32_t>(want);
	}
	case WASM::ValueTypeCode::I64: {
		int64_t got = 0;
		std::memcpy(&got, data, sizeof(got));
		const int64_t want = static_cast<int64_t>(std::strtoll(expected.value.c_str(), nullptr, 10));
		return got == want;
	}
	case WASM::ValueTypeCode::F32: {
		uint32_t got = 0;
		std::memcpy(&got, data, sizeof(got));
		if (expected.value == "nan:canonical")
			return (got & 0x7FFFFFFFu) == 0x7FC00000u;
		if (expected.value == "nan:arithmetic")
			return (got & 0x7FC00000u) == 0x7FC00000u;
		const uint32_t want = static_cast<uint32_t>(std::strtoull(expected.value.c_str(), nullptr, 10));
		return got == want;
	}
	case WASM::ValueTypeCode::F64: {
		uint64_t got = 0;
		std::memcpy(&got, data, sizeof(got));
		if (expected.value == "nan:canonical")
			return (got & 0x7FFFFFFFFFFFFFFFull) == 0x7FF8000000000000ull;
		if (expected.value == "nan:arithmetic")
			return (got & 0x7FF8000000000000ull) == 0x7FF8000000000000ull;
		const uint64_t want = std::strtoull(expected.value.c_str(), nullptr, 10);
		return got == want;
	}
	case WASM::ValueTypeCode::V128:
		return compareV128(expected, data);
	default:
		return false;
	}
}

} // namespace Spec
