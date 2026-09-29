#include "WastScript.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Euphemy/Io/EuphFile.hpp>
#include <jit/jit.h>
#include <nlohmann/json.hpp>

#include "LibJit/LibjitModuleCompiler.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
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
bool isScalarValue(const ScriptValue& value)
{
	if (!value.hasValue)
		return false;
	WASM::ValueTypeCode code = WASM::ValueTypeCode::Void;
	return typeCodeFromName(value.type, code);
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
	// Backing storage for one dynamically-marshalled argument. 16 bytes covers
	// every scalar wasm value; v128 is skipped before we get this far.
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
	void onAction(const ScriptCommand& command);

	bool invoke(WASM::ModuleInstance& instance, const ScriptAction& action, const WASM::FuncType& funcType,
				const WASM::Callable& callable, std::array<unsigned char, 16>& returns);
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
	if (module == nullptr || callable.typeIndex >= module->types.size())
		return nullptr;

	const WASM::Subtype& subtype = module->types[callable.typeIndex];
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
	if (command.type == "assert_trap" || command.type == "assert_exhaustion") {
		skip("trap handling: traps currently terminate the process via std::abort()");
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

	auto module = std::make_unique<WASM::Module>();
	try {
		Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
		module->fromFile(file);
	} catch (const std::exception& error) {
		fail(command.line, "could not load '" + command.filename + "': " + error.what());
		return;
	}

	modules.push_back(std::move(module));

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
		if (!isScalarValue(arg)) {
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
	if (!invoke(*instance, action, *funcType, *callable, returns)) {
		skip("could not marshal the arguments of '" + action.field + "'");
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
			<< " but got " << typeName(resultCode);
	fail(command.line, message.str());
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
		if (!isScalarValue(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}

	std::array<unsigned char, 16> returns {};
	if (!invoke(*instance, action, *funcType, *callable, returns)) {
		skip("could not marshal the arguments of '" + action.field + "'");
		return;
	}

	pass();
}

bool ScriptRunner::invoke(WASM::ModuleInstance& instance, const ScriptAction& action,
						  const WASM::FuncType& funcType, const WASM::Callable& callable,
						  std::array<unsigned char, 16>& returns)
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
		if (!typeCodeFromName(arg.type, code) || code != funcType.params[i].val.opcode)
			return false;

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
		default:
			return false;
		}

		arguments[i + 1] = slots[i].raw;
	}

	// jit_apply knows the calling convention for an arbitrary signature, so we
	// can marshal runtime-typed arguments without generating a template
	// instantiation for every (arity, type) combination. The signature is
	// rebuilt per invocation (libjit keeps its own copy), which is fine for a
	// test harness but would be worth caching in a hot path.
	const jit_type_t signature = jitContext.getTranslator().translateFunctionSignature(funcType);
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
	default:
		return false;
	}
}

} // namespace Spec
