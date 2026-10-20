#include "WastScript.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <csignal>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
#include "LibJit/ComponentRuntime.hpp"
#include "WasmBase/WasmComponent.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmExn.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmStore.hpp"
#include "WasmBase/WasmTrap.hpp"
#include "WasmBase/WasmType.hpp"

namespace Spec {

namespace {

using Json = nlohmann::json;

// The host C ABI's shape for a `v128` argument: 16 bytes, no padding, which
// System V classifies as two INTEGER eightbytes. It is the same mirror the T2
// tests call `V128` in `main.cpp` (docs/SIMD_LIBJIT.md §6.6).
struct AbiV128 { uint32_t lane[4]; };

// Calls `Ret f(void* ctx, AbiV128, …)` for one to four `v128` parameters, i.e.
// the shape of a compiled wasm function wrapper around a ternary SIMD op. Used
// only where `jit_apply` cannot be (see the note in `ScriptRunner::invoke`).
template <typename Ret>
Ret callWithV128Params(void* fnPtr, void* context, const AbiV128* args, size_t count)
{
	switch (count) {
	case 1:
		return reinterpret_cast<Ret (*)(void*, AbiV128)>(fnPtr)(context, args[0]);
	case 2:
		return reinterpret_cast<Ret (*)(void*, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1]);
	case 3:
		return reinterpret_cast<Ret (*)(void*, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2]);
	default:
		return reinterpret_cast<Ret (*)(void*, AbiV128, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2], args[3]);
	}
}

void callWithV128ParamsVoid(void* fnPtr, void* context, const AbiV128* args, size_t count)
{
	switch (count) {
	case 1:
		reinterpret_cast<void (*)(void*, AbiV128)>(fnPtr)(context, args[0]);
		return;
	case 2:
		reinterpret_cast<void (*)(void*, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1]);
		return;
	case 3:
		reinterpret_cast<void (*)(void*, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2]);
		return;
	default:
		reinterpret_cast<void (*)(void*, AbiV128, AbiV128, AbiV128, AbiV128)>(fnPtr)(context, args[0], args[1], args[2], args[3]);
		return;
	}
}

// ── JSON access helpers ─────────────────────────────────────────────────────

bool isComponentFile(const std::string& path)
{
	std::ifstream stream(path, std::ios::binary);
	unsigned char preamble[8]{};
	stream.read(reinterpret_cast<char*>(preamble), sizeof(preamble));
	if (stream.gcount() != static_cast<std::streamsize>(sizeof(preamble)))
		return false;
	return preamble[0] == 0x00 && preamble[1] == 0x61
			&& preamble[2] == 0x73 && preamble[3] == 0x6D
			&& preamble[6] == 0x01 && preamble[7] == 0x00;
}

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
		// `(either ...)`: a list of alternatives the result may match.
		const auto alternatives = json.find("values");
		if (alternatives != json.end() && alternatives->is_array()) {
			for (const Json& alternative : *alternatives)
				value.alternatives.push_back(valueFromJson(alternative));
		}
		// Structured component values.
		if (const auto fields = json.find("fields"); fields != json.end()
				&& fields->is_array()) {
			for (const Json& field : *fields) {
				ScriptValue fieldValue = valueFromJson(field);
				const std::string name = stringOr(field, "name");
				value.fields.emplace_back(name, std::move(fieldValue));
			}
		}
		if (const auto elements = json.find("elements"); elements != json.end()
				&& elements->is_array())
			for (const Json& element : *elements)
				value.elements.push_back(valueFromJson(element));
		if (const auto entries = json.find("entries"); entries != json.end()
				&& entries->is_array()) {
			for (const Json& entry : *entries) {
				ScriptValue key = valueFromJson(entry.value("key", Json{}));
				ScriptValue item = valueFromJson(entry.value("value", Json{}));
				value.entries.emplace_back(std::move(key), std::move(item));
			}
		}
		value.caseLabel = stringOr(json, "case");
		if (const auto payload = json.find("payload"); payload != json.end()) {
			value.payload = std::make_unique<ScriptValue>(valueFromJson(*payload));
			value.hasPayload = true;
		}
		if (const auto flags = json.find("flags"); flags != json.end()
				&& flags->is_array())
			for (const Json& flag : *flags)
				if (flag.is_string())
					value.flags.push_back(flag.get<std::string>());
		if (const auto some = json.find("some"); some != json.end())
			value.some = some->get<bool>();
		if (const auto ok = json.find("ok"); ok != json.end())
			value.ok = ok->get<bool>();
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
	case WASM::ValueTypeCode::ExnRef:
	case WASM::ValueTypeCode::NullExnRef:
		// An exception reference has no host-side constructor; only `null` is
		// marshallable until throw/catch land (EXCEPTIONS.md EX-3).
		return nullptr;
	default:
		// Host-owned: an even, non-null, never-GC-heap pointer.
		return reinterpret_cast<void*>(static_cast<uintptr_t>(0xE0000000ull + 16ull * static_cast<unsigned long long>(raw)));
	}
}

// The reference code a script *type name* denotes. This covers every name the
// converter emits for a reference — including the bottom shorthands — and does
// not require a value payload: `(ref.null)` is emitted as `{"type":"refnull"}`
// with no `value`. Used for arguments *and* expected results (GC.md R5); an
// `exnref`/`nullexnref` is just an opaque pointer for now (EXCEPTIONS.md EX-1).
bool referenceArgTypeCode(const ScriptValue& value, WASM::ValueTypeCode& out)
{
	if (value.type == "funcref") { out = WASM::ValueTypeCode::FuncRef; return true; }
	if (value.type == "externref") { out = WASM::ValueTypeCode::ExternRef; return true; }
	if (value.type == "anyref") { out = WASM::ValueTypeCode::AnyRef; return true; }
	if (value.type == "eqref") { out = WASM::ValueTypeCode::EqRef; return true; }
	if (value.type == "i31ref") { out = WASM::ValueTypeCode::I31Ref; return true; }
	if (value.type == "structref") { out = WASM::ValueTypeCode::StructRef; return true; }
	if (value.type == "arrayref") { out = WASM::ValueTypeCode::ArrayRef; return true; }
	if (value.type == "exnref") { out = WASM::ValueTypeCode::ExnRef; return true; }
	if (value.type == "nullref") { out = WASM::ValueTypeCode::NullRef; return true; }
	if (value.type == "nullfuncref") { out = WASM::ValueTypeCode::NullFuncRef; return true; }
	if (value.type == "nullexternref") { out = WASM::ValueTypeCode::NullExternRef; return true; }
	if (value.type == "nullexnref") { out = WASM::ValueTypeCode::NullExnRef; return true; }
	// `(ref.null)` with no heap type, and `(ref null $t)` for a concrete type.
	if (value.type == "refnull") { out = WASM::ValueTypeCode::NullRef; return true; }
	return false;
}

// True when a script's expected value is a *null* reference. Such a result can
// be verified without a host object model: the returned pointer must be null.
//
// Only two forms are null: an explicit `(ref.null …)` (whose value is "null",
// or whose type is `refnull`), and a bottom-type shorthand (`nullref`,
// `nullfuncref`, `nullexternref`, `nullexnref`) — the sole inhabitant of a
// bottom type is null. A bare `arrayref`/`anyref`/`funcref` without a value is
// a *non-null placeholder*, not a null.
bool isNullReferenceExpectation(const ScriptValue& value)
{
	WASM::ValueTypeCode code = WASM::ValueTypeCode::Void;
	if (!referenceArgTypeCode(value, code))
		return false;
	if (value.value == "null")
		return true;
	if (!value.value.empty())
		return false;
	switch (code) {
	case WASM::ValueTypeCode::NullRef:
	case WASM::ValueTypeCode::NullFuncRef:
	case WASM::ValueTypeCode::NullExternRef:
	case WASM::ValueTypeCode::NullExnRef:
		return true;
	default:
		return false;
	}
}

bool isReferenceArg(const ScriptValue& value)
{
	WASM::ValueTypeCode code = WASM::ValueTypeCode::Void;
	return referenceArgTypeCode(value, code);
}

// Defined below (after the scalar type table).
bool isScalarValue(const ScriptValue& value);

// True for an expected value the harness can verify: a scalar, a reference
// (null, a concrete identity, or a bare `(ref.X)` placeholder), or an `either`
// list whose alternatives are all verifiable (relaxed SIMD).
bool isSupportedExpectation(const ScriptValue& value)
{
	if (value.type == "either") {
		if (value.alternatives.empty())
			return false;
		for (const ScriptValue& alternative : value.alternatives) {
			if (!isSupportedExpectation(alternative))
				return false;
		}
		return true;
	}
	return isScalarValue(value) || isReferenceArg(value);
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
	case WASM::ValueTypeCode::ExnRef:
	case WASM::ValueTypeCode::NullRef:
	case WASM::ValueTypeCode::NullFuncRef:
	case WASM::ValueTypeCode::NullExternRef:
	case WASM::ValueTypeCode::NullExnRef:
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
		if (WASM::isReferenceType(code)) {
			const void* p = nullptr;
			std::memcpy(&p, data, sizeof(p));
			if (p == nullptr)
				out << "null";
			else
				out << p;
			break;
		}
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
		// Measurement mode: execute the four negative-module command kinds
		// instead of skipping them. The default suite stays a conformance run;
		// `WASM_SPEC_VALIDATE=1` is how we see what the decoder/linker already
		// reject and what still needs a validator (docs/TESTING.md).
		strictValidation = std::getenv("WASM_SPEC_VALIDATE") != nullptr;
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
	// See the constructor: `WASM_SPEC_VALIDATE=1` executes the negative-module
	// command kinds and reports real pass/fail instead of skipping them.
	bool strictValidation = false;

	// Imports visible to every module in this script: the `spectest` stubs plus
	// whatever earlier `register` commands exposed.
	WASM::RegistryImportResolver registry;

	// A ModuleInstance holds a raw pointer to its Module, so both containers
	// must keep their contents alive (and stable) for the whole script.
	std::vector<std::unique_ptr<WASM::Module>> modules;
	std::vector<std::unique_ptr<WASM::ModuleInstance>> instances;
	std::vector<std::unique_ptr<WASM::Component>> components;
	std::vector<std::unique_ptr<LibJIT::ComponentRuntime>> componentRuntimes;

	std::unordered_map<std::string, WASM::ModuleInstance*> namedInstances;
	std::unordered_map<std::string, LibJIT::ComponentRuntime*> namedComponents;
	WASM::ModuleInstance* lastInstance = nullptr;
	LibJIT::ComponentRuntime* lastComponent = nullptr;
	bool lastWasComponent = false;

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
	LibJIT::ComponentRuntime* componentFor(const std::string& name, int line);
	std::optional<WASM::FuncType> funcTypeFor(const WASM::ModuleInstance& instance, const WASM::Callable& callable);

	void runCommand(const ScriptCommand& command);
	void onModule(const ScriptCommand& command);
	void onRegister(const ScriptCommand& command);
	void onAssertReturn(const ScriptCommand& command);
	void onAssertTrap(const ScriptCommand& command);
	void onAssertException(const ScriptCommand& command);
	// Negative-module commands (validation / linking / instantiation). They are
	// skipped by default and executed under `WASM_SPEC_VALIDATE=1`.
	enum class ModuleAttempt { Accepted, Rejected, Trapped };
	ModuleAttempt attemptModule(const ScriptCommand& command, bool instantiate);
	void onAssertMalformed(const ScriptCommand& command);
	void onAssertInvalid(const ScriptCommand& command);
	void onAssertUnlinkable(const ScriptCommand& command);
	void onAssertUninstantiable(const ScriptCommand& command);
	void onAction(const ScriptCommand& command);
	void onAssertExhaustion(const ScriptCommand& command);

	bool invoke(WASM::ModuleInstance& instance, const ScriptAction& action, const WASM::FuncType& funcType,
				const WASM::Callable& callable, std::array<unsigned char, 16>& returns, bool& trapped,
				std::vector<unsigned char>* tuple = nullptr);
	bool compareResult(const ScriptValue& expected, WASM::ValueTypeCode actual, const unsigned char* data,
					   WASM::ModuleInstance& instance);
	bool compareReferenceResult(const ScriptValue& expected, WASM::ValueTypeCode actual,
								const unsigned char* data, WASM::ModuleInstance& instance);
};

} // namespace

WitFunction witFunctionFromJson(const Json& json)
{
	WitFunction function;
	function.name = stringOr(json, "name");
	if (json.is_object()) {
		const auto params = json.find("params");
		if (params != json.end() && params->is_array())
			for (const Json& param : *params)
				function.params.emplace_back(stringOr(param, "name"),
						stringOr(param, "type"));
		const auto result = json.find("result");
		if (result != json.end() && result->is_string())
			function.result = result->get<std::string>();
	}
	return function;
}

WitPackage WitPackage::parse(std::string_view json)
{
	const Json root = Json::parse(json);
	WitPackage package;
	if (const auto packages = root.find("packages");
			packages != root.end() && packages->is_array() && !packages->empty())
		package.name = stringOr((*packages)[0], "name");

	if (const auto interfaces = root.find("interfaces");
			interfaces != root.end() && interfaces->is_array()) {
		for (const Json& entry : *interfaces) {
			WitInterface interface;
			interface.name = stringOr(entry, "name");
			if (const auto functions = entry.find("functions");
					functions != entry.end() && functions->is_object())
				for (const auto& [name, function] : functions->items())
					interface.functions.emplace(name,
							witFunctionFromJson(function));
			package.interfaces.push_back(std::move(interface));
		}
	}

	if (const auto worlds = root.find("worlds");
			worlds != root.end() && worlds->is_array()) {
		for (const Json& entry : *worlds) {
			WitWorld world;
			world.name = stringOr(entry, "name");
			if (const auto imports = entry.find("imports");
					imports != entry.end() && imports->is_object()) {
				for (const auto& [name, import] : imports->items()) {
					std::string description = "<function>";
					const auto interface = import.find("interface");
					if (interface != import.end()) {
						const int id = intOr(*interface, "id");
						if (id >= 0
								&& std::size_t(id) < package.interfaces.size())
							description = package.interfaces[std::size_t(id)].name;
					}
					world.imports.emplace(name, description);
				}
			}
			if (const auto exports = entry.find("exports");
					exports != entry.end() && exports->is_object())
				for (const auto& [name, exportEntry] : exports->items()) {
					const auto function = exportEntry.find("function");
					if (function != exportEntry.end())
						world.exports.emplace(name, witFunctionFromJson(*function));
				}
			package.worlds.push_back(std::move(world));
		}
	}
	return package;
}

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
	// Publish the host table's type too: `imports.wast` checks that a module
	// importing `spectest.table` with incompatible limits is rejected.
	WASM::TableType spectestTableType{};
	spectestTableType.elementType = WASM::ValueType{WASM::ValueTypeCode::FuncRef, -1};
	spectestTableType.limits.flags = 0x01; // has a maximum
	spectestTableType.limits.initial = 10;
	spectestTableType.limits.maximum = 20;
	registry.registerTable("spectest", "table",
			WASM::ImportedTable{spectestTable, spectestTableType});

	static WASM::LinearMemory* spectestMemory = nullptr;
	if (spectestMemory == nullptr)
		spectestMemory = store.memory(store.createLinearMemory(1, 2, false));
	registry.registerMemory("spectest", "memory", spectestMemory);

	// The 64-bit-index variants. The index type is part of the memory's type and
	// is matched at link time now, so `spectest.memory64` must be a real
	// memory64 (docs/proposals/MEMORY64.md M2).
	static WASM::TableInstance* spectestTable64 = nullptr;
	if (spectestTable64 == nullptr)
		spectestTable64 = store.table(store.createTable(10, 20));
	registry.registerTable("spectest", "table64", WASM::ImportedTable{spectestTable64});

	static WASM::LinearMemory* spectestMemory64 = nullptr;
	if (spectestMemory64 == nullptr)
		spectestMemory64 = store.memory(store.createLinearMemory(1, 2, false, true));
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

LibJIT::ComponentRuntime* ScriptRunner::componentFor(const std::string& name,
		int line)
{
	if (!name.empty()) {
		const auto it = namedComponents.find(name);
		if (it == namedComponents.end()) {
			fail(line, "action refers to unknown component '" + name + "'");
			return nullptr;
		}
		return it->second;
	}
	if (lastComponent == nullptr) {
		fail(line, "no component has been instantiated yet");
		return nullptr;
	}
	return lastComponent;
}

std::optional<WASM::FuncType> ScriptRunner::funcTypeFor(const WASM::ModuleInstance& instance, const WASM::Callable& callable)
{
	const WASM::Module* module = instance.context()->module;
	if (module == nullptr || callable.localTypeIdx >= module->typeCount())
		return std::nullopt;
	if (module->typeView(callable.localTypeIdx).kind() != WASM::CanonTypeKind::Func)
		return std::nullopt;
	return module->funcTypeAt(callable.localTypeIdx);
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

	// Command families that are deliberately not executed in a normal run. They
	// are reported as skipped so the suite stays actionable while the engine
	// grows instead of drowning real regressions in expected failures.
	if (command.type == "assert_exhaustion") {
		onAssertExhaustion(command);
		return;
	}
	if (command.type == "assert_trap") {
		onAssertTrap(command);
		return;
	}
	// Negative-module commands are split by *why* the module should be refused:
	// the decoder (malformed), a validator (invalid), the linker (unlinkable),
	// or a trapping start function (uninstantiable). Only `assert_invalid`
	// genuinely needs a validating front end; the harness executes all four
	// under `WASM_SPEC_VALIDATE=1` and skips them (by kind) otherwise.
	if (command.type == "assert_malformed") {
		onAssertMalformed(command);
		return;
	}
	if (command.type == "assert_invalid") {
		onAssertInvalid(command);
		return;
	}
	if (command.type == "assert_unlinkable") {
		onAssertUnlinkable(command);
		return;
	}
	if (command.type == "assert_uninstantiable") {
		onAssertUninstantiable(command);
		return;
	}
	if (command.type == "module_definition") {
		// Module-linking `(module definition …)`: decoded but *not* instantiated.
		// The core suite uses it to declare limits that cannot be allocated
		// (`(module definition (memory 65536))`, `(table i64 0xffff… )`).
		if (command.moduleType != "binary") {
			skip("text-format module definition '" + command.filename + "'");
			return;
		}
		if (attemptModule(command, false) == ModuleAttempt::Accepted)
			pass();
		else
			fail(command.line, "could not decode module definition ('" + command.filename + "')");
		return;
	}
	if (command.type == "module_instance") {
		skip("module-linking proposal: module_instance");
		return;
	}
	if (command.type == "assert_exception") {
		onAssertException(command);
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

	if (isComponentFile(path)) {
		auto component = std::make_unique<WASM::Component>();
		try {
			Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
			component->fromFile(file);
		} catch (const std::exception& error) {
			fail(command.line, "could not load component '" + command.filename
					+ "': " + error.what());
			return;
		}
		components.push_back(std::move(component));
		try {
			auto runtime = std::make_unique<LibJIT::ComponentRuntime>(
					jitContext.rawContext(), *components.back());
			lastComponent = runtime.get();
			lastWasComponent = true;
			if (!command.name.empty())
				namedComponents[command.name] = lastComponent;
			componentRuntimes.push_back(std::move(runtime));
		} catch (const std::exception& error) {
			fail(command.line, "component instantiation of '" + command.filename
					+ "' failed: " + error.what());
			return;
		}
		pass();
		return;
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
	lastWasComponent = false;

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

const WASM::TypeStore& componentStore()
{
return WASM::Store::global().types().store();
}

bool componentTypeViewFromValue(const WASM::ComponentValueType& type,
WASM::ComponentTypeView& view)
{
if (type.kind != WASM::ComponentValueType::Kind::TypeId
|| WASM::TypeId{type.typeIndex}.isNone())
return false;
view = componentStore().componentType(WASM::TypeId{type.typeIndex});
return true;
}

bool componentPrimitiveFromScript(const ScriptValue& value,
WASM::PrimitiveValueType primitive, LibJIT::ComponentValue& out)
{
using K = LibJIT::ComponentValue::Kind;
using P = WASM::PrimitiveValueType;
switch (primitive) {
case P::Bool:
out.kind = K::Bool;
out.u64 = value.value == "true" || value.value == "1";
return true;
case P::S8: out.kind = K::S8; out.s64 = std::strtoll(value.value.c_str(), nullptr, 10); return true;
case P::U8: out.kind = K::U8; out.u64 = std::strtoull(value.value.c_str(), nullptr, 10); return true;
case P::S16: out.kind = K::S16; out.s64 = std::strtoll(value.value.c_str(), nullptr, 10); return true;
case P::U16: out.kind = K::U16; out.u64 = std::strtoull(value.value.c_str(), nullptr, 10); return true;
case P::S32: out.kind = K::S32; out.s64 = std::strtoll(value.value.c_str(), nullptr, 10); return true;
case P::U32: out.kind = K::U32; out.u64 = std::strtoull(value.value.c_str(), nullptr, 10); return true;
case P::S64: out.kind = K::S64; out.s64 = std::strtoll(value.value.c_str(), nullptr, 10); return true;
case P::U64: out.kind = K::U64; out.u64 = std::strtoull(value.value.c_str(), nullptr, 10); return true;
case P::F32: {
const uint32_t bits = uint32_t(std::strtoull(value.value.c_str(), nullptr, 10));
out.kind = K::F32;
float number;
std::memcpy(&number, &bits, sizeof(number));
out.f64 = number;
return true;
}
case P::F64: {
const uint64_t bits = std::strtoull(value.value.c_str(), nullptr, 10);
out.kind = K::F64;
std::memcpy(&out.f64, &bits, sizeof(bits));
return true;
}
case P::Char:
out.kind = K::Char;
out.discriminant = uint32_t(std::strtoul(value.value.c_str(), nullptr, 10));
return true;
case P::String:
out.kind = K::String;
out.string = value.value;
return true;
default:
return false;
}
}

// Converts one script value against the canonical component type it is being
// passed/compared as. Structured values are ordered by the *type*, not by the
// script text, so record field order and variant labels are checked.
bool componentValueFromScript(const ScriptValue& value,
const WASM::ComponentValueType& type, LibJIT::ComponentValue& out)
{
using K = LibJIT::ComponentValue::Kind;
if (type.kind == WASM::ComponentValueType::Kind::Primitive)
return componentPrimitiveFromScript(value, type.primitive, out);

WASM::ComponentTypeView view;
if (!componentTypeViewFromValue(type, view))
return false;

switch (view.kind()) {
case WASM::TypeKind::ComponentPrimitive:
return componentPrimitiveFromScript(value, view.primitive(), out);
case WASM::TypeKind::ComponentRecord: {
out.kind = K::Record;
out.elements.resize(view.fields().size());
for (std::size_t i = 0; i < view.fields().size(); ++i) {
const std::string name(componentStore().string(view.fields()[i].name));
const ScriptValue* field = nullptr;
for (const auto& candidate : value.fields)
if (candidate.first == name)
field = &candidate.second;
if (field == nullptr
|| !componentValueFromScript(*field, view.fields()[i].type,
out.elements[i]))
return false;
}
return true;
}
case WASM::TypeKind::ComponentList:
case WASM::TypeKind::ComponentFixedList: {
out.kind = view.kind() == WASM::TypeKind::ComponentList ? K::List
: K::FixedList;
out.elements.resize(value.elements.size());
if (view.kind() == WASM::TypeKind::ComponentFixedList
&& value.elements.size() != view.fixedLength())
return false;
for (std::size_t i = 0; i < value.elements.size(); ++i)
if (!componentValueFromScript(value.elements[i], view.element(),
out.elements[i]))
return false;
return true;
}
case WASM::TypeKind::ComponentTuple: {
out.kind = K::Tuple;
out.elements.resize(view.tuple().size());
if (value.elements.size() != view.tuple().size())
return false;
for (std::size_t i = 0; i < view.tuple().size(); ++i)
if (!componentValueFromScript(value.elements[i], view.tuple()[i],
out.elements[i]))
return false;
return true;
}
case WASM::TypeKind::ComponentVariant: {
out.kind = K::Variant;
out.discriminant = 0;
bool found = false;
for (std::size_t i = 0; i < view.cases().size(); ++i) {
if (componentStore().string(view.cases()[i].label) != value.caseLabel)
continue;
out.discriminant = static_cast<uint32_t>(i);
found = true;
if (view.cases()[i].hasType) {
if (!value.hasPayload || !value.payload)
return false;
LibJIT::ComponentValue payloadValue;
if (!componentValueFromScript(*value.payload,
view.cases()[i].type, payloadValue))
return false;
out.elements.resize(1);
out.elements[0] = std::move(payloadValue);
out.hasValue = true;
}
break;
}
return found;
}
case WASM::TypeKind::ComponentEnum: {
out.kind = K::Enum;
out.discriminant = 0;
for (std::size_t i = 0; i < view.labels().size(); ++i)
if (componentStore().string(view.labels()[i]) == value.value) {
out.discriminant = static_cast<uint32_t>(i);
return true;
}
return false;
}
case WASM::TypeKind::ComponentFlags: {
out.kind = K::Flags;
out.flags.assign(view.labels().size(), false);
for (const std::string& flag : value.flags) {
bool found = false;
for (std::size_t i = 0; i < view.labels().size(); ++i)
if (componentStore().string(view.labels()[i]) == flag) {
out.flags[i] = true;
found = true;
break;
}
if (!found)
return false;
}
return true;
}
case WASM::TypeKind::ComponentOption:
out.kind = K::Option;
out.hasValue = value.some;
if (!value.some)
return true;
if (!value.payload)
return false;
out.elements.resize(1);
return componentValueFromScript(*value.payload, view.element(),
out.elements[0]);
case WASM::TypeKind::ComponentResult:
out.kind = K::Result;
out.discriminant = value.ok ? 0u : 1u;
out.hasValue = value.hasPayload;
if (!value.hasPayload)
return true;
if (!value.payload)
return false;
out.elements.resize(1);
return componentValueFromScript(*value.payload,
value.ok ? view.resultOk() : view.resultError(), out.elements[0]);
case WASM::TypeKind::ComponentMap:
out.kind = K::Map;
out.elements.resize(value.entries.size());
for (std::size_t i = 0; i < value.entries.size(); ++i) {
out.elements[i].kind = K::Tuple;
out.elements[i].elements.resize(2);
if (!componentValueFromScript(value.entries[i].first, view.mapKey(),
out.elements[i].elements[0])
|| !componentValueFromScript(value.entries[i].second,
view.mapValue(), out.elements[i].elements[1]))
return false;
}
return true;
case WASM::TypeKind::ComponentOwn:
case WASM::TypeKind::ComponentBorrow:
out.kind = view.kind() == WASM::TypeKind::ComponentOwn ? K::Own : K::Borrow;
out.discriminant = uint32_t(std::strtoul(value.value.c_str(), nullptr, 10));
return true;
case WASM::TypeKind::ComponentStream:
case WASM::TypeKind::ComponentFuture:
out.kind = view.kind() == WASM::TypeKind::ComponentStream ? K::Stream : K::Future;
out.discriminant = uint32_t(std::strtoul(value.value.c_str(), nullptr, 10));
return true;
default:
return false;
}
}

bool sameComponentValue(const LibJIT::ComponentValue& actual,
const LibJIT::ComponentValue& expected)
{
using K = LibJIT::ComponentValue::Kind;
if (actual.kind != expected.kind)
return false;
switch (actual.kind) {
case K::String: return actual.string == expected.string;
case K::F32:
case K::F64: return actual.f64 == expected.f64;
case K::Bool:
case K::S8: case K::U8: case K::S16: case K::U16:
case K::S32: case K::U32: case K::S64: case K::U64:
return actual.u64 == expected.u64;
case K::Char: case K::Variant: case K::Enum:
case K::Own: case K::Borrow: case K::Stream: case K::Future:
return actual.discriminant == expected.discriminant;
default: break;
}
if (actual.flags != expected.flags || actual.hasValue != expected.hasValue)
return false;
if (actual.elements.size() != expected.elements.size())
return false;
for (std::size_t i = 0; i < actual.elements.size(); ++i)
if (!sameComponentValue(actual.elements[i], expected.elements[i]))
return false;
return true;
}

bool componentValueMatches(const LibJIT::ComponentValue& actual,
const ScriptValue& expected, const WASM::ComponentValueType& type)
{
if (!expected.alternatives.empty()) {
for (const ScriptValue& alternative : expected.alternatives)
if (componentValueMatches(actual, alternative, type))
return true;
return false;
}
LibJIT::ComponentValue wanted;
if (!componentValueFromScript(expected, type, wanted))
return false;
const bool same = sameComponentValue(actual, wanted);
return same;
}

void ScriptRunner::onAssertReturn(const ScriptCommand& command)
{
	const ScriptAction& action = command.action;

	if (action.type != "invoke") {
		skip("assert_return with a non-invoke action ('" + action.type + "')");
		return;
	}

	const bool componentAction = action.module.empty()
			? lastWasComponent : namedComponents.count(action.module) != 0;
	if (componentAction) {
		LibJIT::ComponentRuntime* component = componentFor(action.module, command.line);
		if (component == nullptr)
			return;
		const auto function = component->exportedFunction(action.field);
		if (!function.has_value()) {
			fail(command.line, "no component function named '" + action.field + "'");
			return;
		}
		if (!function->callValues) {
			skip("component function '" + action.field + "' has no structured entry point");
			return;
		}
		if (function->paramTypes.size() != action.args.size()) {
			fail(command.line, "component arity mismatch for '" + action.field + "'");
			return;
		}
		if (function->resultTypes.size() != command.expected.size()) {
			fail(command.line, "component result count mismatch for '" + action.field + "'");
			return;
		}
		std::vector<LibJIT::ComponentValue> args(action.args.size());
		for (std::size_t i = 0; i < action.args.size(); ++i) {
			if (!componentValueFromScript(action.args[i],
					function->paramTypes[i], args[i])) {
				skip("component argument type '" + action.args[i].type + "' is not supported");
				return;
			}
		}
		std::vector<LibJIT::ComponentValue> results(function->resultTypes.size());
		try {
			function->callValues(args, results);
		} catch (const std::exception& error) {
			fail(command.line, "component invocation of '" + action.field
					+ "' failed: " + error.what());
			return;
		}
		for (std::size_t i = 0; i < results.size(); ++i) {
			if (!componentValueMatches(results[i], command.expected[i],
					function->resultTypes[i])) {
				fail(command.line, "component assert_return failed for '"
						+ action.field + "' result #" + std::to_string(i));
				return;
			}
		}
		pass();
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

	auto funcType = funcTypeFor(*instance, *callable);
	if (!funcType) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}

	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
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
		if (isSupportedExpectation(expected))
			continue;
		skip("expected type '" + expected.type + "' is not supported");
		return;
	}

	if (funcType->results.size() > 1) {
		std::array<unsigned char, 16> unused {};
		std::vector<unsigned char> tuple;
		bool trapped = false;
		if (!invoke(*instance, action, *funcType, *callable, unused, trapped, &tuple)) {
			skip("could not marshal the arguments of '" + action.field + "'");
			return;
		}
		if (trapped) {
			fail(command.line, "unexpected trap from '" + action.field + "': "
					+ std::string(lastTrapMessage != nullptr ? lastTrapMessage : "trap"));
			return;
		}
		if (::wasm_exn_take_pending() != nullptr) {
			fail(command.line, "uncaught exception from '" + action.field + "'");
			return;
		}

		// The callee wrote its results into the caller-provided tuple, each at
		// the offset of the corresponding result type (docs/ABI.md).
		const jit_type_t tupleType = jitContext.getTranslator().translateStruct(funcType->results);
		for (size_t i = 0; i < command.expected.size(); ++i) {
			const jit_type_t fieldType = jit_type_get_field(tupleType, static_cast<unsigned int>(i));
			const size_t offset = static_cast<size_t>(
					jit_type_get_offset(tupleType, static_cast<unsigned int>(i)));
			const size_t fieldSize = static_cast<size_t>(jit_type_get_size(fieldType));
			if (offset + fieldSize > tuple.size()) {
				skip("multi-result value is larger than the marshalling buffer");
				return;
			}
			const WASM::ValueTypeCode code = funcType->results[i].val.opcode;
			if (compareResult(command.expected[i], code, tuple.data() + offset, *instance))
				continue;

			std::stringstream message;
			message << "assert_return failed for '" << action.field << "' result #" << i
					<< ": expected " << command.expected[i].type << " "
					<< command.expected[i].value << " but got " << typeName(code) << " "
					<< formatResultValue(code, tuple.data() + offset);
			fail(command.line, message.str());
			return;
		}
		pass();
		return;
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
	if (::wasm_exn_take_pending() != nullptr) {
		fail(command.line, "uncaught exception from '" + action.field + "'");
		return;
	}

	if (command.expected.empty()) {
		pass();
		return;
	}

	const WASM::ValueTypeCode resultCode = funcType->results[0].val.opcode;
	if (compareResult(command.expected[0], resultCode, returns.data(), *instance)) {
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

	auto funcType = funcTypeFor(*instance, *callable);
	if (!funcType) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}
	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
		return;
	}
	for (const ScriptValue& arg : action.args) {
		if (!isScalarValue(arg) && !isReferenceArg(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}

	std::array<unsigned char, 16> returns {};
	std::vector<unsigned char> tuple;
	bool trapped = false;
	if (!invoke(*instance, action, *funcType, *callable, returns, trapped, funcType->results.size() > 1 ? &tuple : nullptr)) {
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
	if (::wasm_exn_take_pending() != nullptr) {
		fail(command.line, "expected a trap from '" + action.field + "' but it threw an exception");
		return;
	}
	fail(command.line, "expected a trap from '" + action.field + "' but it returned normally");
}

void ScriptRunner::onAssertException(const ScriptCommand& command)
{
	// A wasm exception is a *caught or pending* non-local exit, not a trap:
	// `invoke` returns normally and leaves the exception in the pending slot
	// (docs/proposals/EXCEPTIONS.md EXD1). A trap here is a failure (a trap is
	// not catchable), as is a normal return.
	const ScriptAction& action = command.action;

	if (action.type != "invoke") {
		skip("assert_exception with a non-invoke action ('" + action.type + "')");
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

	auto funcType = funcTypeFor(*instance, *callable);
	if (!funcType) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}
	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
		return;
	}
	for (const ScriptValue& arg : action.args) {
		if (!isScalarValue(arg) && !isReferenceArg(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}

	std::array<unsigned char, 16> returns {};
	std::vector<unsigned char> tuple;
	bool trapped = false;
	if (!invoke(*instance, action, *funcType, *callable, returns, trapped, funcType->results.size() > 1 ? &tuple : nullptr)) {
		skip("could not marshal the arguments of '" + action.field + "'");
		return;
	}

	if (trapped) {
		fail(command.line, "expected an exception from '" + action.field + "' but it trapped ('" +
				WASM::trapKindName(lastTrapKind) + "')");
		return;
	}

	// Take (and clear) the pending exception: a stale one must not leak into
	// the next command.
	if (::wasm_exn_take_pending() == nullptr) {
		fail(command.line, "expected an exception from '" + action.field + "' but it returned normally");
		return;
	}
	pass();
}

ScriptRunner::ModuleAttempt ScriptRunner::attemptModule(const ScriptCommand& command, bool instantiate)
{
	std::string path = wasmDir;
	if (!path.empty() && path.back() != '/')
		path.push_back('/');
	path += command.filename;

	auto module = std::make_unique<WASM::Module>();
	try {
		Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
		module->fromFile(file);
	} catch (const std::exception& error) {
		if (std::getenv("WJ_TRACE") != nullptr)
			std::fprintf(stderr, "[wj]   rejected %s: %s\n", command.filename.c_str(), error.what());
		return ModuleAttempt::Rejected;
	}
	if (!instantiate)
		return ModuleAttempt::Accepted;

	// Keep the module alive: an instance holds a raw `const Module*`.
	modules.push_back(std::move(module));

	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
	std::unique_ptr<WASM::ModuleInstance> instance;
	// A start function may trap during instantiation; that is the expected
	// outcome for `assert_uninstantiable`, not a host failure.
	WASM::TrapScope trapScope;
	if (setjmp(trapScope.buffer) != 0) {
		lastTrapKind = trapScope.kind();
		lastTrapMessage = trapScope.message();
		return ModuleAttempt::Trapped;
	}
	try {
		instance = compiler.instantiate(*modules.back(), registry);
	} catch (const std::exception& error) {
		if (std::getenv("WJ_TRACE") != nullptr)
			std::fprintf(stderr, "[wj]   link rejected %s: %s\n", command.filename.c_str(), error.what());
		return ModuleAttempt::Rejected;
	}
	if (instance == nullptr)
		return ModuleAttempt::Rejected;
	// Keep it alive for the script's lifetime (it may own Store references), but
	// it is deliberately not the target of later `invoke`s.
	instances.push_back(std::move(instance));
	return ModuleAttempt::Accepted;
}

void ScriptRunner::onAssertMalformed(const ScriptCommand& command)
{
	if (!strictValidation) {
		skip("assert_malformed: decoder rejection not exercised by default "
			 "(set WASM_SPEC_VALIDATE=1 to measure)");
		return;
	}
	if (command.moduleType != "binary") {
		skip("text-format module '" + command.filename + "' (only binary modules can be loaded)");
		return;
	}
	const ModuleAttempt attempt = attemptModule(command, false);
	std::fprintf(stderr, "[validate] assert_malformed %s %s\n",
			attempt == ModuleAttempt::Rejected ? "rejected" : "accepted",
			command.filename.c_str());
	if (attempt == ModuleAttempt::Rejected)
		pass();
	else
		fail(command.line, "accepted a malformed module ('" + command.filename + "')");
}

void ScriptRunner::onAssertInvalid(const ScriptCommand& command)
{
	if (!strictValidation) {
		skip("assert_invalid: negative-module commands not executed by default "
			 "(set WASM_SPEC_VALIDATE=1 to measure; see docs/VALIDATION.md)");
		return;
	}
	if (command.moduleType != "binary") {
		skip("text-format module '" + command.filename + "' (only binary modules can be loaded)");
		return;
	}
	const ModuleAttempt attempt = attemptModule(command, false);
	std::fprintf(stderr, "[validate] assert_invalid %s %s\n",
			attempt == ModuleAttempt::Rejected ? "rejected" : "accepted",
			command.filename.c_str());
	if (attempt == ModuleAttempt::Rejected)
		pass();
	else
		fail(command.line, "accepted an invalid module ('" + command.filename + "')");
}

void ScriptRunner::onAssertUnlinkable(const ScriptCommand& command)
{
	if (!strictValidation) {
		skip("assert_unlinkable: link failures not verified by default "
			 "(set WASM_SPEC_VALIDATE=1 to measure)");
		return;
	}
	if (command.moduleType != "binary") {
		skip("text-format module '" + command.filename + "' (only binary modules can be loaded)");
		return;
	}
	const ModuleAttempt attempt = attemptModule(command, true);
	std::fprintf(stderr, "[validate] assert_unlinkable %s %s\n",
			attempt == ModuleAttempt::Rejected ? "rejected"
					: (attempt == ModuleAttempt::Trapped ? "trapped" : "accepted"),
			command.filename.c_str());
	if (attempt == ModuleAttempt::Rejected)
		pass();
	else
		fail(command.line, "instantiated an unlinkable module ('" + command.filename + "')");
}

void ScriptRunner::onAssertUninstantiable(const ScriptCommand& command)
{
	if (!strictValidation) {
		skip("assert_uninstantiable: start-function traps not verified by default "
			 "(set WASM_SPEC_VALIDATE=1 to measure)");
		return;
	}
	if (command.moduleType != "binary") {
		skip("text-format module '" + command.filename + "' (only binary modules can be loaded)");
		return;
	}
	// A trap (the usual case) or a thrown instantiation error both mean the
	// module did not become usable.
	const ModuleAttempt attempt = attemptModule(command, true);
	std::fprintf(stderr, "[validate] assert_uninstantiable %s %s\n",
			attempt == ModuleAttempt::Accepted ? "accepted"
					: (attempt == ModuleAttempt::Trapped ? "trapped" : "rejected"),
			command.filename.c_str());
	if (attempt == ModuleAttempt::Rejected || attempt == ModuleAttempt::Trapped)
		pass();
	else
		fail(command.line, "instantiated an uninstantiable module ('" + command.filename + "')");
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

	auto funcType = funcTypeFor(*instance, *callable);
	if (!funcType) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}

	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
		return;
	}

	for (const ScriptValue& arg : action.args) {
		if (!isScalarValue(arg) && !isReferenceArg(arg)) {
			skip("argument type '" + arg.type + "' is not supported");
			return;
		}
	}

	std::array<unsigned char, 16> returns {};
	// A bare action discards the results, but a multi-result callee still needs
	// a caller-owned buffer in the JIT ABI (docs/ABI.md; MV1).
	std::vector<unsigned char> tuple;
	bool trapped = false;
	if (!invoke(*instance, action, *funcType, *callable, returns, trapped, funcType->results.size() > 1 ? &tuple : nullptr)) {
		skip("could not marshal the arguments of '" + action.field + "'");
		return;
	}
	if (trapped) {
		fail(command.line, "unexpected trap from '" + action.field + "': "
				+ std::string(lastTrapMessage != nullptr ? lastTrapMessage : "trap"));
		return;
	}

	if (::wasm_exn_take_pending() != nullptr) {
		fail(command.line, "uncaught exception from '" + action.field + "'");
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

	auto funcType = funcTypeFor(*instance, *callable);
	if (!funcType) {
		skip("export '" + action.field + "' does not have a plain function type");
		return;
	}
	if (funcType->params.size() != action.args.size()) {
		fail(command.line, "arity mismatch for '" + action.field + "'");
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
		std::vector<unsigned char> tuple;
		bool trapped = false;
		invoke(*instance, action, *funcType, *callable, returns, trapped, funcType->results.size() > 1 ? &tuple : nullptr);
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
						  std::array<unsigned char, 16>& returns, bool& trapped,
						  std::vector<unsigned char>* tuple)
{
	// A pending exception from a previous (failed) assertion must not leak into
	// this invocation.
	::wasm_exn_clear_pending();

	// A multi-result function's JIT signature takes a result buffer as a
	// trailing parameter (docs/ABI.md). Every result-discarding caller passes
	// an empty buffer too; callers that compare results pass their own.
	if (funcType.results.size() > 1 && tuple == nullptr)
		return false;

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

	// Multi-result callee: the JIT signature has one extra trailing `void*`
	// parameter pointing at the result tuple (docs/ABI.md). The caller owns
	// the buffer; the callee writes its results through it.
	jit_type_t tupleType = nullptr;
	void* tupleAddress = nullptr;
	if (tuple != nullptr && funcType.results.size() > 1) {
		tupleType = jitContext.getTranslator().translateStruct(funcType.results);
		const size_t size = static_cast<size_t>(jit_type_get_size(tupleType));
		if (jit_type_get_alignment(tupleType) > alignof(std::max_align_t))
			return false; // a vector's storage is max_align_t-aligned
		tuple->assign(std::max<size_t>(size, 16), 0);
		tupleAddress = tuple->data();
		// jit_apply wants a pointer to each argument *value* (like arguments[0],
		// which points at the `context` local), not the value itself.
		arguments.push_back(&tupleAddress);
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

	// libjit's `jit_apply` damages a third stack-passed 16-byte aggregate
	// (docs/SIMD_LIBJIT.md §6.6). The *callee* side is verified correct — the T2
	// case for `simd_abi_args.wat` passes 2, 3 and 4 `v128` parameters through
	// the host ABI and reads them back exactly — so the defect is in the
	// dynamic-call shim, not in the generated code. Every ternary SIMD op is
	// invoked in precisely that shape, which is why sixteen spec scripts could
	// not be graded at all.
	//
	// So those calls do not go through `jit_apply`: an all-`v128` signature with a
	// single `v128` result is called through the C ABI directly, which is the ABI
	// the callee was generated for and the one `WASM::callCallable` uses. The
	// arguments are already marshalled into 16-byte slots by this point.
	const auto v128AbiCall = [&]() {
		if (tuple != nullptr)
			return false;
		if (funcType.params.empty() || funcType.params.size() > 4)
			return false;
		for (const auto& param : funcType.params)
			if (param.val.opcode != WASM::ValueTypeCode::V128)
				return false;
		// The result may be absent, a `v128`, or one scalar — `v128.bitselect`
		// wrapped in a comparison returns `i32`, and that shape was the last
		// failing group in `simd_boolean.wast`.
		WASM::ValueTypeCode resultCode = WASM::ValueTypeCode::Void;
		if (funcType.results.size() == 1) {
			resultCode = funcType.results[0].val.opcode;
			const bool ok = resultCode == WASM::ValueTypeCode::V128
					|| resultCode == WASM::ValueTypeCode::I32
					|| resultCode == WASM::ValueTypeCode::I64
					|| resultCode == WASM::ValueTypeCode::F32
					|| resultCode == WASM::ValueTypeCode::F64;
			if (!ok)
				return false;
		} else if (funcType.results.size() > 1) {
			return false;
		}

		AbiV128 args[4];
		for (size_t i = 0; i < funcType.params.size(); ++i)
			std::memcpy(&args[i], slots[i].raw, sizeof(AbiV128));

		const size_t count = funcType.params.size();
		void* fnPtr = callable.fnPtr;
		switch (resultCode) {
		case WASM::ValueTypeCode::V128: {
			const AbiV128 result = callWithV128Params<AbiV128>(fnPtr, context, args, count);
			std::memcpy(returns.data(), &result, sizeof(result));
			break;
		}
		case WASM::ValueTypeCode::I32: {
			const int32_t result = callWithV128Params<int32_t>(fnPtr, context, args, count);
			std::memcpy(returns.data(), &result, sizeof(result));
			break;
		}
		case WASM::ValueTypeCode::I64: {
			const int64_t result = callWithV128Params<int64_t>(fnPtr, context, args, count);
			std::memcpy(returns.data(), &result, sizeof(result));
			break;
		}
		case WASM::ValueTypeCode::F32: {
			const float result = callWithV128Params<float>(fnPtr, context, args, count);
			std::memcpy(returns.data(), &result, sizeof(result));
			break;
		}
		case WASM::ValueTypeCode::F64: {
			const double result = callWithV128Params<double>(fnPtr, context, args, count);
			std::memcpy(returns.data(), &result, sizeof(result));
			break;
		}
		default:
			callWithV128ParamsVoid(fnPtr, context, args, count);
			break;
		}
		return true;
	};
	if (v128AbiCall())
		return true;

	const unsigned int numArgs = static_cast<unsigned int>(funcType.params.size() + 1)
			+ (tupleType != nullptr ? 1u : 0u);
	jit_apply(signature, callable.fnPtr, arguments.data(), numArgs,
			  tupleType != nullptr ? nullptr : returns.data());
	return true;
}

bool ScriptRunner::compareReferenceResult(const ScriptValue& expected, WASM::ValueTypeCode actual,
										  const unsigned char* data, WASM::ModuleInstance& instance)
{
	if (!WASM::isReferenceType(actual))
		return false;
	const void* got = nullptr;
	std::memcpy(&got, data, sizeof(got));

	// `(ref.null …)` and the bottom shorthands: the result must be null.
	if (isNullReferenceExpectation(expected))
		return got == nullptr;

	// Every other reference expectation is non-null.
	if (got == nullptr)
		return false;

	WASM::ValueTypeCode code = WASM::ValueTypeCode::Void;
	if (!referenceArgTypeCode(expected, code))
		return false;

	if (!expected.value.empty() && expected.value != "null") {
		// A concrete identity: `(ref.extern N)`, `(ref.i31 K)`, `(ref.func N)`.
		// Reusing the argument mapping keeps `ref.eq` consistent (GC.md R4/R5).
		return got == referenceArgument(instance, code, expected);
	}

	// Bare placeholders. `(ref.func)` promises a real function reference, which
	// this engine represents as a registered `Callable*`; `(ref.array)` and the
	// other bare forms only promise a non-null reference, which is all the
	// harness can check without a host object model.
	if (code == WASM::ValueTypeCode::FuncRef)
		return WASM::Store::global().callableAt(got) != nullptr;
	return true;
}

bool ScriptRunner::compareResult(const ScriptValue& expected, WASM::ValueTypeCode actual,
								 const unsigned char* data, WASM::ModuleInstance& instance)
{
	// `(either ...)`: the script accepts any one alternative (relaxed SIMD).
	if (expected.type == "either") {
		for (const ScriptValue& alternative : expected.alternatives) {
			if (compareResult(alternative, actual, data, instance))
				return true;
		}
		return false;
	}

	// Reference results: null is exact, a concrete identity uses the argument
	// mapping, a bare placeholder checks what it promises (GC.md R5).
	if (isReferenceArg(expected))
		return compareReferenceResult(expected, actual, data, instance);

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
