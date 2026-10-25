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
	// The sequence belongs to the state, not to the caller: a record must be
	// comparable with one emitted by a different boundary.
	diagnostic.sequence = ++sequence;

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

std::string diagnosticsToJson(std::span<const Diagnostic> diagnostics)
{
	nlohmann::ordered_json json = nlohmann::ordered_json::array();
	for (const Diagnostic& diagnostic : diagnostics)
		json.push_back(diagnosticJson(diagnostic));
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

