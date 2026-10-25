#include "WasmDiagnostics.hpp"
#include "WasmAgent.hpp"
#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include "WasmNativeImport.hpp"
#include "WasmVMContext.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace WASM {

// ── Names ──────────────────────────────────────────────────────────────────

const char* diagnosticSeverityName(DiagnosticSeverity severity) noexcept
{
	switch (severity) {
	case DiagnosticSeverity::Note:    return "note";
	case DiagnosticSeverity::Warning: return "warning";
	case DiagnosticSeverity::Error:   return "error";
	case DiagnosticSeverity::Fatal:   return "fatal";
	}
	return "error";
}

const char* diagnosticPhaseName(DiagnosticPhase phase) noexcept
{
	switch (phase) {
	case DiagnosticPhase::Decode:       return "decode";
	case DiagnosticPhase::Validate:     return "validate";
	case DiagnosticPhase::Compile:      return "compile";
	case DiagnosticPhase::Instantiate:  return "instantiate";
	case DiagnosticPhase::Execute:      return "execute";
	case DiagnosticPhase::Continuation: return "continuation";
	case DiagnosticPhase::Component:    return "component";
	}
	return "execute";
}

const char* continuationProviderName(ContinuationProvider provider) noexcept
{
	switch (provider) {
	case ContinuationProvider::NativeStacks: return "native_stacks";
	case ContinuationProvider::HostThreads:  return "host_threads";
	}
	return "native_stacks";
}

const char* continuationStatusName(ContinuationStatus status) noexcept
{
	switch (status) {
	case ContinuationStatus::Completed: return "completed";
	case ContinuationStatus::Suspended: return "suspended";
	case ContinuationStatus::Trapped:   return "trapped";
	case ContinuationStatus::Exception: return "exception";
	case ContinuationStatus::Invalid:   return "invalid";
	}
	return "invalid";
}

// ── Stable id mappings ─────────────────────────────────────────────────────
//
// One arm per enumerator: adding a `TrapKind`/`ContinuationStatus` without an
// id is a compile error in a -Wswitch build and a test failure otherwise. The
// spellings mirror `trapKindName()` upper-cased with underscores.

std::string_view trapDiagnosticId(TrapKind kind) noexcept
{
	switch (kind) {
	case TrapKind::Unreachable:                return "WASM.EXECUTE.TRAP.UNREACHABLE";
	case TrapKind::NullReference:              return "WASM.EXECUTE.TRAP.NULL_REFERENCE";
	case TrapKind::NullFunctionReference:      return "WASM.EXECUTE.TRAP.NULL_FUNCTION_REFERENCE";
	case TrapKind::NullStructureReference:     return "WASM.EXECUTE.TRAP.NULL_STRUCTURE_REFERENCE";
	case TrapKind::NullArrayReference:         return "WASM.EXECUTE.TRAP.NULL_ARRAY_REFERENCE";
	case TrapKind::NullI31Reference:           return "WASM.EXECUTE.TRAP.NULL_I31_REFERENCE";
	case TrapKind::IntegerDivideByZero:        return "WASM.EXECUTE.TRAP.INTEGER_DIVIDE_BY_ZERO";
	case TrapKind::IntegerOverflow:            return "WASM.EXECUTE.TRAP.INTEGER_OVERFLOW";
	case TrapKind::InvalidConversionToInteger: return "WASM.EXECUTE.TRAP.INVALID_CONVERSION";
	case TrapKind::InvalidString:              return "WASM.EXECUTE.TRAP.INVALID_STRING";
	case TrapKind::OutOfBoundsMemoryAccess:    return "WASM.EXECUTE.TRAP.OUT_OF_BOUNDS_MEMORY";
	case TrapKind::OutOfBoundsTableAccess:     return "WASM.EXECUTE.TRAP.OUT_OF_BOUNDS_TABLE";
	case TrapKind::OutOfBoundsArrayAccess:     return "WASM.EXECUTE.TRAP.OUT_OF_BOUNDS_ARRAY";
	case TrapKind::UnalignedAtomicAccess:      return "WASM.EXECUTE.TRAP.UNALIGNED_ATOMIC";
	case TrapKind::UninitializedElement:       return "WASM.EXECUTE.TRAP.UNINITIALIZED_ELEMENT";
	case TrapKind::UndefinedElement:           return "WASM.EXECUTE.TRAP.UNDEFINED_ELEMENT";
	case TrapKind::IndirectCallTypeMismatch:   return "WASM.EXECUTE.TRAP.INDIRECT_CALL_TYPE_MISMATCH";
	case TrapKind::CastFailure:                return "WASM.EXECUTE.TRAP.CAST_FAILURE";
	case TrapKind::OutOfMemory:                return "WASM.EXECUTE.TRAP.OUT_OF_MEMORY";
	case TrapKind::CallStackExhausted:         return "WASM.EXECUTE.TRAP.CALL_STACK_EXHAUSTED";
	case TrapKind::Internal:                   return "WASM.EXECUTE.TRAP.INTERNAL";
	}
	return "WASM.EXECUTE.TRAP.INTERNAL";
}

std::string_view continuationDiagnosticId(ContinuationStatus status) noexcept
{
	switch (status) {
	case ContinuationStatus::Completed: return "WASM.CONTINUATION.COMPLETED";
	case ContinuationStatus::Suspended: return "WASM.CONTINUATION.SUSPENDED";
	case ContinuationStatus::Trapped:   return "WASM.CONTINUATION.TRAPPED";
	case ContinuationStatus::Exception: return "WASM.CONTINUATION.EXCEPTION";
	case ContinuationStatus::Invalid:   return "WASM.CONTINUATION.INVALID";
	}
	return "WASM.CONTINUATION.INVALID";
}

// ── Validator rule families (DG-0) ─────────────────────────────────────────
//
// 247 `fail()` sites exist; most pass only a message. Tagging each one by hand
// would be churn with no behavioural gain, so this ordered classifier maps the
// rule's *message* to a stable family. Explicit ids still win at the site (the
// index rules and `funcTypeAt` use them). Matching is ASCII-case-insensitive
// and allocation-free; the order below is the contract (docs/DIAGNOSTICS.md
// §4.2) — the first matching family wins.

namespace {

bool containsLower(std::string_view haystack, std::string_view needle) noexcept
{
	if (needle.empty() || needle.size() > haystack.size())
		return false;
	for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		bool match = true;
		for (std::size_t j = 0; j < needle.size(); ++j) {
			char left = haystack[i + j];
			char right = needle[j];
			if (left >= 'A' && left <= 'Z')
				left = static_cast<char>(left - 'A' + 'a');
			if (right >= 'A' && right <= 'Z')
				right = static_cast<char>(right - 'A' + 'a');
			if (left != right) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

} // namespace

std::string_view validationRuleId(std::string_view message) noexcept
{
	if (containsLower(message, "unknown label"))
		return DiagnosticId::kValidateUnknownLabel;
	if (containsLower(message, "unknown type"))
		return DiagnosticId::kValidateUnknownType;
	if (containsLower(message, "not a function type"))
		return DiagnosticId::kValidateTypeNotFunction;
	if (containsLower(message, "unknown function"))
		return DiagnosticId::kValidateUnknownFunction;
	if (containsLower(message, "unknown table"))
		return DiagnosticId::kValidateUnknownTable;
	if (containsLower(message, "unknown memory"))
		return DiagnosticId::kValidateUnknownMemory;
	if (containsLower(message, "unknown global"))
		return DiagnosticId::kValidateUnknownGlobal;
	if (containsLower(message, "unknown tag"))
		return DiagnosticId::kValidateUnknownTag;
	if (containsLower(message, "unknown local"))
		return DiagnosticId::kValidateUnknownLocal;
	if (containsLower(message, "unknown element segment"))
		return DiagnosticId::kValidateUnknownElement;
	if (containsLower(message, "unknown data segment"))
		return DiagnosticId::kValidateUnknownData;

	// The stack check is a distinct rule even when the message says
	// "type mismatch: operand stack underflow".
	if (containsLower(message, "operand stack"))
		return DiagnosticId::kValidateOperandStack;
	if (containsLower(message, "type mismatch"))
		return DiagnosticId::kValidateTypeMismatch;
	if (containsLower(message, "block type"))
		return DiagnosticId::kValidateBlockType;
	if (containsLower(message, "else without")
			|| containsLower(message, "end without")
			|| containsLower(message, "if without")
			|| containsLower(message, "instruction outside"))
		return DiagnosticId::kValidateControlFlow;
	if (containsLower(message, "alignment"))
		return DiagnosticId::kValidateAlignment;
	if (containsLower(message, "offset out of range"))
		return DiagnosticId::kValidateOffsetRange;
	if (containsLower(message, "limits")
			|| containsLower(message, "size minimum")
			|| containsLower(message, "size maximum")
			|| containsLower(message, "declare a maximum"))
		return DiagnosticId::kValidateLimits;
	if (containsLower(message, "immutable") || containsLower(message, "mutable"))
		return DiagnosticId::kValidateImmutableTarget;
	if (containsLower(message, "uninitialized local"))
		return DiagnosticId::kValidateUninitializedLocal;
	if (containsLower(message, "undeclared function reference"))
		return DiagnosticId::kValidateUndeclaredRef;
	if (containsLower(message, "duplicate export"))
		return DiagnosticId::kValidateDuplicateExport;
	if (containsLower(message, "section") || containsLower(message, "data count"))
		return DiagnosticId::kValidateSection;
	if (containsLower(message, "constant-time") || containsLower(message, "secret"))
		return DiagnosticId::kValidateConstantTime;
	if (containsLower(message, "profile"))
		return DiagnosticId::kValidateProfile;
	if (containsLower(message, "descriptor"))
		return DiagnosticId::kValidateDescriptor;
	if (containsLower(message, "cast") || containsLower(message, "ref.test"))
		return DiagnosticId::kValidateCast;
	if (containsLower(message, "ordering"))
		return DiagnosticId::kValidateAtomicOrdering;
	if (containsLower(message, "atomic"))
		return DiagnosticId::kValidateAtomic;
	if (containsLower(message, "continuation") || containsLower(message, "cont.bind")
			|| containsLower(message, "resume") || containsLower(message, "suspend")
			|| containsLower(message, "switch") || containsLower(message, "handler"))
		return DiagnosticId::kValidateContinuation;
	if (containsLower(message, "string"))
		return DiagnosticId::kValidateString;
	if (containsLower(message, "struct"))
		return DiagnosticId::kValidateStruct;
	if (containsLower(message, "array"))
		return DiagnosticId::kValidateArray;
	if (containsLower(message, "tag"))
		return DiagnosticId::kValidateTag;
	if (containsLower(message, "table"))
		return DiagnosticId::kValidateTable;
	if (containsLower(message, "memory"))
		return DiagnosticId::kValidateMemory;
	if (containsLower(message, "global"))
		return DiagnosticId::kValidateGlobal;
	if (containsLower(message, "local"))
		return DiagnosticId::kValidateLocal;
	if (containsLower(message, "select"))
		return DiagnosticId::kValidateSelect;
	if (containsLower(message, "shuffle"))
		return DiagnosticId::kValidateShuffle;
	if (containsLower(message, "is not a") || containsLower(message, "is not an")
			|| containsLower(message, "has no"))
		return DiagnosticId::kValidateTypeKind;
	return DiagnosticId::kValidateRule;
}

// ── The bounded ring ───────────────────────────────────────────────────────

void DiagnosticState::setCapacity(std::uint32_t newCapacity) noexcept
{
	capacity = newCapacity;
	try {
		// Keep the newest records; a capacity of zero drops them all.
		if (newCapacity == 0) {
			records.clear();
		} else if (records.size() > newCapacity) {
			records.erase(records.begin(),
					records.begin() + static_cast<std::ptrdiff_t>(records.size() - newCapacity));
		}
	} catch (...) {
		// A shrinking operation only destroys storage; nothing to recover.
	}
}

void DiagnosticState::emit(Diagnostic diagnostic) noexcept
{
	// The sequence and the wall-clock stamp belong to the state, not to the
	// caller: a record must be comparable with one emitted by a different
	// boundary, and JSON keeps the raw number rather than a rendered date.
	diagnostic.sequence = ++sequence;
	diagnostic.timestamp = std::time(nullptr);

	// Pretty terminal output is opt-in and restricted to the host-log imports:
	// the spec suite deliberately produces thousands of traps/validation
	// failures, and those belong in the ring/sink, not on stderr.
	if (prettyPrintLogs
			&& diagnostic.id.rfind("WASM.EXECUTE.LOG.", 0) == 0) {
		try {
			const std::string line = formatDiagnosticPretty(diagnostic);
			std::fwrite(line.data(), 1, line.size(), stderr);
			std::fflush(stderr);
		} catch (...) {
			// A terminal line is best-effort; the record below still lands.
		}
	}

	// Notify first, with the local record. A sink that re-enters the engine (or
	// calls `emitDiagnostic` again) therefore cannot invalidate the object it is
	// reading, and the callback never observes a partially updated vector.
	if (sink != nullptr) {
		// The contract is `noexcept`: a callback that throws terminates the
		// process before this frame can react, so there is nothing to catch.
		sink(userData, diagnostic);
	}

	if (capacity != 0) {
		try {
			if (records.size() >= capacity)
				records.erase(records.begin());
			records.push_back(std::move(diagnostic));
		} catch (...) {
			// Losing a debugging record is preferable to failing the run it
			// describes (docs/SAFETY.md §2.4: a diagnostic is not a trap).
		}
	}
}

void setDiagnosticSink(DiagnosticSink sink, void* userData) noexcept
{
	currentExecutionState().diagnostics.setSink(sink, userData);
}

DiagnosticSink diagnosticSink() noexcept
{
	return currentExecutionState().diagnostics.sink;
}

void* diagnosticSinkUserData() noexcept
{
	return currentExecutionState().diagnostics.userData;
}

std::span<const Diagnostic> recentDiagnostics() noexcept
{
	return currentExecutionState().diagnostics.recent();
}

void clearDiagnostics() noexcept
{
	currentExecutionState().diagnostics.clear();
}

void setDiagnosticRingCapacity(std::uint32_t capacity) noexcept
{
	currentExecutionState().diagnostics.setCapacity(capacity);
}

void setDiagnosticPrettyPrinting(bool enabled) noexcept
{
	currentExecutionState().diagnostics.prettyPrintLogs = enabled;
}

bool diagnosticPrettyPrinting() noexcept
{
	return currentExecutionState().diagnostics.prettyPrintLogs;
}

std::time_t diagnosticsStartedAt() noexcept
{
	return currentExecutionState().diagnostics.startedAt;
}

void emitDiagnostic(Diagnostic diagnostic) noexcept
{
	currentExecutionState().diagnostics.emit(std::move(diagnostic));
}

// ── JSON ───────────────────────────────────────────────────────────────────
//
// nlohmann_json is the project's JSON library (Test/WastScript.cpp already uses
// it), so diagnostics use it too rather than a second, hand-rolled escape
// routine. The include lives here, not in the public header: an embedder that
// only consumes records does not pull the JSON library into its translation
// units.
//
// `ordered_json` keeps the documented field order. The serializer never throws
// on guest bytes: `dump` uses `error_handler_t::replace`, so invalid UTF-8 in a
// log message becomes U+FFFD instead of a `type_error.316`.

namespace {

nlohmann::ordered_json diagnosticJson(const Diagnostic& diagnostic)
{
	nlohmann::ordered_json json;
	json["schema"] = std::string(kDiagnosticJsonSchema);
	json["sequence"] = diagnostic.sequence;
	json["timestamp"] = static_cast<std::int64_t>(diagnostic.timestamp);
	json["id"] = std::string(diagnostic.id);
	json["severity"] = diagnosticSeverityName(diagnostic.severity);
	json["phase"] = diagnosticPhaseName(diagnostic.phase);
	json["message"] = diagnostic.message;

	if (diagnostic.location.known()) {
		nlohmann::ordered_json location;
		if (diagnostic.location.functionIndex != kNoDiagnosticIndex)
			location["function"] = diagnostic.location.functionIndex;
		if (!diagnostic.location.functionName.empty())
			location["name"] = diagnostic.location.functionName;
		if (diagnostic.location.instructionOffset != kNoDiagnosticIndex)
			location["instruction"] = diagnostic.location.instructionOffset;
		if (diagnostic.location.moduleOffset != kNoDiagnosticOffset)
			location["moduleByte"] = diagnostic.location.moduleOffset;
		json["location"] = std::move(location);
	}

	if (diagnostic.continuation.present()) {
		nlohmann::ordered_json continuation;
		continuation["provider"] =
				continuationProviderName(diagnostic.continuation.provider);
		continuation["generation"] = diagnostic.continuation.generation;
		continuation["state"] = std::string(diagnostic.continuation.state);
		if (diagnostic.continuation.tagId >= 0)
			continuation["tag"] = diagnostic.continuation.tagId;
		continuation["values"] = diagnostic.continuation.valueCount;
		continuation["results"] = diagnostic.continuation.resultCount;
		continuation["roots"] = diagnostic.continuation.hasRoots;
		json["continuation"] = std::move(continuation);
	}

	if (!diagnostic.remediation.empty())
		json["remediation"] = std::string(diagnostic.remediation);
	return json;
}

std::string dumpJson(const nlohmann::ordered_json& json)
{
	return json.dump(-1, ' ', false,
			nlohmann::ordered_json::error_handler_t::replace);
}

} // namespace

std::string diagnosticToJson(const Diagnostic& diagnostic)
{
	return dumpJson(diagnosticJson(diagnostic));
}

std::string diagnosticsToJson(std::span<const Diagnostic> diagnostics,
		std::time_t startedAt)
{
	if (startedAt == 0) {
		if (!diagnostics.empty())
			startedAt = diagnostics.front().timestamp;
		if (startedAt == 0)
			startedAt = std::time(nullptr);
	}

	nlohmann::ordered_json json;
	json["schema"] = std::string(kDiagnosticJsonSchema);
	json["startedAt"] = static_cast<std::int64_t>(startedAt);
	nlohmann::ordered_json records = nlohmann::ordered_json::array();
	for (const Diagnostic& diagnostic : diagnostics)
		records.push_back(diagnosticJson(diagnostic));
	json["records"] = std::move(records);
	return dumpJson(json);
}

// ── Text form ──────────────────────────────────────────────────────────────

std::string formatDiagnostic(const Diagnostic& diagnostic)
{
	std::string out;
	out.reserve(160);
	out += diagnosticSeverityName(diagnostic.severity);
	out.push_back(' ');
	if (!diagnostic.id.empty()) {
		out += diagnostic.id;
		out += ": ";
	}
	out += diagnostic.message;

	if (diagnostic.location.known()) {
		out += "\n  at";
		if (diagnostic.location.functionIndex != kNoDiagnosticIndex) {
			out += " function ";
			out += std::to_string(diagnostic.location.functionIndex);
		}
		if (!diagnostic.location.functionName.empty()) {
			out += " ($";
			out += diagnostic.location.functionName;
			out.push_back(')');
		}
		if (diagnostic.location.instructionOffset != kNoDiagnosticIndex) {
			out += " instruction ";
			out += std::to_string(diagnostic.location.instructionOffset);
		}
		if (diagnostic.location.moduleOffset != kNoDiagnosticOffset) {
			char offset[32];
			std::snprintf(offset, sizeof(offset), " (module byte 0x%llx)",
					static_cast<unsigned long long>(diagnostic.location.moduleOffset));
			out += offset;
		}
	}

	if (diagnostic.continuation.present()) {
		out += "\n  continuation ";
		out += continuationProviderName(diagnostic.continuation.provider);
		out += " generation ";
		out += std::to_string(diagnostic.continuation.generation);
		out += " state ";
		out += diagnostic.continuation.state;
		if (diagnostic.continuation.tagId >= 0) {
			out += " tag ";
			out += std::to_string(diagnostic.continuation.tagId);
		}
	}

	if (!diagnostic.remediation.empty()) {
		out += "\n  hint: ";
		out += diagnostic.remediation;
	}
	return out;
}

// ── Pretty terminal form ───────────────────────────────────────────────────
//
// One line per host-log record. The emoji is uncoloured; the ANSI colour starts
// right after it and ends at the line's end (reset before the newline). The
// timestamp is rendered with `ctime`, which is exactly what the raw JSON number
// deliberately does not do.

namespace {

const char* severityEmoji(DiagnosticSeverity severity) noexcept
{
	switch (severity) {
	case DiagnosticSeverity::Note:    return "\xE2\x84\xB9\xEF\xB8\x8F"; // ℹ️
	case DiagnosticSeverity::Warning: return "\xE2\x9A\xA0\xEF\xB8\x8F"; // ⚠️
	case DiagnosticSeverity::Error:   return "\xE2\x9B\x94";             // ⛔
	case DiagnosticSeverity::Fatal:   return "\xF0\x9F\x9A\xA8";         // 🚨
	}
	return "\xE2\x9B\x94";
}

const char* severityColor(DiagnosticSeverity severity) noexcept
{
	switch (severity) {
	case DiagnosticSeverity::Note:    return "\x1b[37m"; // white
	case DiagnosticSeverity::Warning: return "\x1b[33m"; // yellow
	case DiagnosticSeverity::Error:   return "\x1b[31m"; // red
	case DiagnosticSeverity::Fatal:   return "\x1b[35m"; // magenta
	}
	return "\x1b[31m";
}

// `ctime` format (`Www Mmm dd hh:mm:ss yyyy`), without its trailing newline.
// `ctime_r` keeps worker-thread emissions from racing on the shared buffer.
std::string formatDiagnosticTime(std::time_t value)
{
	char buffer[32] = {};
#if defined(__unix__) || defined(__APPLE__)
	if (ctime_r(&value, buffer) == nullptr)
		return "unknown time";
#else
	const char* text = std::ctime(&value);
	if (text == nullptr)
		return "unknown time";
	std::snprintf(buffer, sizeof(buffer), "%s", text);
#endif
	std::string out(buffer);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
		out.pop_back();
	return out;
}

// A terminal line must stay readable even when the guest sent a 4096-byte
// buffer full of NULs (the truncation test does exactly that). The record keeps
// the full message; the pretty form is capped and control bytes become spaces.
constexpr std::size_t kPrettyMessageBytes = 256;

std::string prettyMessage(std::string_view message)
{
	const std::size_t bytes = std::min(message.size(), kPrettyMessageBytes);
	std::string out;
	out.reserve(bytes + 3);
	for (std::size_t i = 0; i < bytes; ++i) {
		const unsigned char byte = static_cast<unsigned char>(message[i]);
		out.push_back(byte < 0x20 || byte == 0x7F ? ' ' : static_cast<char>(byte));
	}
	if (message.size() > kPrettyMessageBytes)
		out += "...";
	return out;
}

} // namespace

std::string formatDiagnosticPretty(const Diagnostic& diagnostic, bool useColor)
{
	std::string out;
	out.reserve(std::min<std::size_t>(diagnostic.message.size(), kPrettyMessageBytes) + 64);
	out += severityEmoji(diagnostic.severity);
	out.push_back(' ');
	if (useColor)
		out += severityColor(diagnostic.severity);
	out += '[';
	out += formatDiagnosticTime(diagnostic.timestamp != 0
			? diagnostic.timestamp : std::time(nullptr));
	out += "] ";
	out += prettyMessage(diagnostic.message);
	if (useColor)
		out += "\x1b[0m";
	out.push_back('\n');
	return out;
}

// ── Boundary helpers ───────────────────────────────────────────────────────

void emitDecodeFailure(std::string_view message) noexcept
{
	Diagnostic diagnostic;
	// The decoder throws `std::runtime_error` with a named prefix. Until DG-0
	// maps each site, this boundary is the honest classification: a message that
	// names an unsupported/unimplemented feature is an unsupported input; every
	// other decode failure is malformed input.
	const bool unsupported = message.find("unsupported") != std::string_view::npos
			|| message.find("Unsupported") != std::string_view::npos
			|| message.find("not implemented") != std::string_view::npos;
	diagnostic.id = unsupported ? DiagnosticId::kDecodeUnsupported
								: DiagnosticId::kDecodeMalformed;
	diagnostic.severity = DiagnosticSeverity::Error;
	diagnostic.phase = DiagnosticPhase::Decode;
	diagnostic.remediation = "docs/VALIDATION.md";
	try {
		diagnostic.message = std::string(message);
		emitDiagnostic(std::move(diagnostic));
	} catch (...) {
		// Best-effort: a diagnostic must not replace the decode failure.
	}
}

void emitExceptionDiagnostic(std::int32_t tagId, std::uint32_t payloadBytes) noexcept
{
	Diagnostic diagnostic;
	diagnostic.id = DiagnosticId::kExecuteException;
	diagnostic.severity = DiagnosticSeverity::Error;
	diagnostic.phase = DiagnosticPhase::Execute;
	diagnostic.remediation = "docs/proposals/EXCEPTIONS.md";
	try {
		diagnostic.message = "uncaught exception (tag " + std::to_string(tagId)
				+ ", " + std::to_string(payloadBytes) + " payload bytes)";
		emitDiagnostic(std::move(diagnostic));
	} catch (...) {
		// Best-effort, as above.
	}
}

void emitTrapDiagnostic(TrapKind kind, const char* message,
		std::uint32_t functionIndex, std::uint32_t instructionOffset,
		const char* functionName) noexcept
{
	Diagnostic diagnostic;
	diagnostic.id = trapDiagnosticId(kind);
	diagnostic.severity = DiagnosticSeverity::Error;
	diagnostic.phase = DiagnosticPhase::Execute;
	diagnostic.remediation = "docs/ABI.md#native-host-function-imports";
	diagnostic.location.functionIndex = functionIndex;
	diagnostic.location.instructionOffset = instructionOffset;
	try {
		if (functionName != nullptr)
			diagnostic.location.functionName = functionName;
		diagnostic.message = (message != nullptr) ? message : trapKindName(kind);
		emitDiagnostic(std::move(diagnostic));
	} catch (...) {
		// A diagnostic is never allowed to change the trap it describes.
	}
}

void emitContinuationDiagnostic(std::string_view id, DiagnosticSeverity severity,
		std::string message, DiagnosticContinuation continuation,
		std::string_view remediation) noexcept
{
	Diagnostic diagnostic;
	diagnostic.id = id;
	diagnostic.severity = severity;
	diagnostic.phase = DiagnosticPhase::Continuation;
	diagnostic.remediation = remediation;
	try {
		diagnostic.message = std::move(message);
		diagnostic.continuation = std::move(continuation);
		emitDiagnostic(std::move(diagnostic));
	} catch (...) {
		// A failed allocation must not replace the trap/status being described.
	}
}

// ── Guest log imports ──────────────────────────────────────────────────────

namespace {

// Reads a UTF-8 message out of the module's memory 0. Everything that could
// make the range unreadable degrades to a deterministic placeholder: a log
// import must never trap on a bad pointer, because the module under test is
// exactly the thing being diagnosed.
std::string guestMessage(VMContext* context, std::int32_t pointer, std::int32_t length)
{
	if (length < 0 || pointer < 0)
		return "<guest string: negative range>";
	if (context == nullptr || context->memories == nullptr || context->memoryCount == 0
			|| context->memories[0] == nullptr)
		return "<guest string: module has no linear memory>";

	const LinearMemory* memory = context->memories[0];
	const auto base = static_cast<std::uint64_t>(static_cast<std::uint32_t>(pointer));
	const auto requested = static_cast<std::uint64_t>(static_cast<std::uint32_t>(length));
	const std::uint64_t size = wasm_memory_size(memory);
	if (memory->memoryBase == nullptr || base > size || requested > size - base)
		return "<guest string: range outside linear memory>";

	const std::uint64_t readable = std::min<std::uint64_t>(requested, kMaxDiagnosticMessageBytes);
	std::string message(reinterpret_cast<const char*>(memory->memoryBase + base),
			static_cast<std::size_t>(readable));
	if (readable < requested)
		message += "...";
	return message;
}

void emitHostLog(DiagnosticSeverity severity, std::string_view id,
		VMContext* context, std::int32_t pointer, std::int32_t length) noexcept
{
	Diagnostic diagnostic;
	diagnostic.id = id;
	diagnostic.severity = severity;
	diagnostic.phase = DiagnosticPhase::Execute;
	diagnostic.remediation = "docs/DIAGNOSTICS.md#54-host-log-imports";
	try {
		diagnostic.message = guestMessage(context, pointer, length);
		emitDiagnostic(std::move(diagnostic));
	} catch (...) {
		// The logging facility is best-effort; an allocation failure must not
		// turn a diagnostic into a different failure.
	}
}

} // namespace

void diagnosticInfo(VMContext* context, std::int32_t pointer, std::int32_t length)
{
	emitHostLog(DiagnosticSeverity::Note, DiagnosticId::kExecuteLogInfo, context, pointer, length);
}

void diagnosticWarn(VMContext* context, std::int32_t pointer, std::int32_t length)
{
	emitHostLog(DiagnosticSeverity::Warning, DiagnosticId::kExecuteLogWarn, context, pointer, length);
}

void diagnosticError(VMContext* context, std::int32_t pointer, std::int32_t length)
{
	emitHostLog(DiagnosticSeverity::Error, DiagnosticId::kExecuteLogError, context, pointer, length);
}

void diagnosticPanic(VMContext* context, std::int32_t pointer, std::int32_t length)
{
	std::string message;
	try {
		message = guestMessage(context, pointer, length);
	} catch (...) {
		message = "<guest panic>";
	}

	Diagnostic diagnostic;
	diagnostic.id = DiagnosticId::kExecuteLogPanic;
	diagnostic.severity = DiagnosticSeverity::Fatal;
	diagnostic.phase = DiagnosticPhase::Execute;
	diagnostic.message = message;
	diagnostic.remediation = "docs/DIAGNOSTICS.md#54-host-log-imports";
	emitDiagnostic(std::move(diagnostic));

	// A guest panic is a deliberate host-defined failure, not a spec trap. It
	// is reported through the trap slot like any other import failure so the
	// generated caller returns instead of unwinding; `nativeImport` (the only
	// supported way to register these bodies) gives `WASM::trap` a catch frame.
	trap(TrapKind::Internal, message.c_str());
}

void registerDiagnosticImports(ImportRegistrar& registrar, std::string_view moduleName)
{
	registrar.registerFunction(moduleName, "info", nativeImport<&diagnosticInfo>());
	registrar.registerFunction(moduleName, "warn", nativeImport<&diagnosticWarn>());
	registrar.registerFunction(moduleName, "error", nativeImport<&diagnosticError>());
	registrar.registerFunction(moduleName, "panic", nativeImport<&diagnosticPanic>());
}

} // namespace WASM

