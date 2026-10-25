#ifndef WASMDIAGNOSTICS_HPP
#define WASMDIAGNOSTICS_HPP

#include "WasmContinuation.hpp"
#include "WasmTrap.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace WASM {

// ── Structured diagnostics (docs/DIAGNOSTICS.md) ────────────────────────────
//
// Today the engine reports failures through several unrelated channels: a
// named validation error, `TrapKind` plus a copied message, a pending
// exception, a `ContinuationStatus`, and provider/scheduler state. Each is
// useful on its own, but nothing correlates them into one record a test,
// an embedder or a tool can consume without parsing prose.
//
// This header is that correlation layer, and it is backend-neutral: a record
// carries facts the core owns (id, severity, phase, module-local location,
// continuation context), never a backend type or host pointer. Backends feed
// it through the same functions the core uses, so a diagnostic looks the same
// whichever code generator produced the failing code.
//
// Design rules (docs/DIAGNOSTICS.md §4):
//
//   * `id` is the contract. Prose in `message` may improve without breaking an
//     assertion; changing an id is a breaking change and needs a doc/test edit.
//   * Emission collects at *boundaries* — the validator rule, the trap slot's
//     conversion to a host exception, the pending-exception take, the four
//     guest log imports. It is not threaded through every helper.
//   * The ring is bounded and per execution state. A null sink with a zero
//     capacity is the zero-cost configuration (no allocation on the path at
//     all). The default capacity is deliberately small: diagnostics are a
//     debugging surface, not a log file.
//   * A sink is `noexcept` and must not throw; the engine cannot unwind a
//     generated frame. It is called with the record itself *before* the record
//     is appended to the ring, so a sink that re-enters the engine cannot
//     invalidate what it is reading. `recentDiagnostics()` inside a sink does
//     not yet include the record being delivered.
//   * A record is self-contained: the message and the function name are owned
//     strings, so a record copied into the ring or a sink outlives the module
//     that produced it. The only `string_view`s are static literals (id,
//     remediation, continuation state).

// Severity is the severity of the *record*, not of the outcome. A `Note` from
// `env.info` is still a successful call; a `Fatal` from `env.panic` is followed
// by a trap. `Error` is a recoverable-as-a-trap failure (validation, a Wasm
// trap, an uncaught exception). `Fatal` means the host facility itself declared
// the run unusable.
enum class DiagnosticSeverity : std::uint8_t { Note, Warning, Error, Fatal };

// The pipeline stage that produced the record. It is intentionally coarse: a
// consumer switches on it, and a new stage is a deliberate addition.
enum class DiagnosticPhase : std::uint8_t {
	Decode,
	Validate,
	Compile,
	Instantiate,
	Execute,
	Continuation,
	Component,
};

// "note" / "warning" / "error" / "fatal" — the text and JSON spelling.
const char* diagnosticSeverityName(DiagnosticSeverity severity) noexcept;
// "decode" / "validate" / … / "component".
const char* diagnosticPhaseName(DiagnosticPhase phase) noexcept;
// "native_stacks" / "host_threads".
const char* continuationProviderName(ContinuationProvider provider) noexcept;

// Absence markers. An all-unknown location is omitted from the text/JSON form
// rather than serialized as 0 (a real function/offset is a valid 0).
inline constexpr std::uint32_t kNoDiagnosticIndex = 0xFFFFFFFFu;
inline constexpr std::uint64_t kNoDiagnosticOffset = ~std::uint64_t{0};

// Where the failure happened, in module terms. `functionIndex` is module-local
// (imports first, then defined functions) and `instructionOffset` counts from
// the start of the function body. `moduleOffset` is the byte offset in the
// binary when the decode path knows it. `functionName` is the name-section
// spelling without `$`; it is *owned* (not borrowed), so a record copied into
// the ring or a sink stays valid after the module is destroyed.
struct DiagnosticLocation {
	std::uint32_t functionIndex = kNoDiagnosticIndex;
	std::uint32_t instructionOffset = kNoDiagnosticIndex;
	std::uint64_t moduleOffset = kNoDiagnosticOffset;
	std::string functionName;

	bool known() const noexcept
	{
		return functionIndex != kNoDiagnosticIndex
				|| instructionOffset != kNoDiagnosticIndex
				|| moduleOffset != kNoDiagnosticOffset;
	}
};

// Continuation context for a trap, uncaught exception or scheduler failure
// (docs/DIAGNOSTICS.md §4.3). It is populated when `state` is non-empty; that
// is the single "present" marker, so a record that is not continuation-related
// stays cheap and is serialized without a `continuation` object.
//
// `switchPeer` is developer-only: it is never serialized and never included in
// the text form. Default JSON contains no host pointers, guest values or
// `userData`.
struct DiagnosticContinuation {
	ContinuationProvider provider = ContinuationProvider::NativeStacks;
	std::uint64_t generation = 0;
	std::string_view state;   // "running", "suspended", "completed", "trapped", "exception"
	std::int32_t tagId = -1;
	std::uint32_t valueCount = 0;
	std::uint32_t resultCount = 0;
	bool hasRoots = false;
	const void* switchPeer = nullptr;

	bool present() const noexcept { return !state.empty(); }
};

struct Diagnostic {
	std::string_view id;          // stable contract, e.g. "WASM.VALIDATE.UNKNOWN_TYPE"
	DiagnosticSeverity severity = DiagnosticSeverity::Error;
	DiagnosticPhase phase = DiagnosticPhase::Execute;
	std::string message;          // human-readable; not an assertion key
	DiagnosticLocation location;
	DiagnosticContinuation continuation;
	std::string_view remediation; // docs anchor or a short hint
	std::uint32_t sequence = 0;   // assigned by the emitting execution state
};

// ── Stable identifiers ─────────────────────────────────────────────────────
//
// The taxonomy is documented in docs/DIAGNOSTICS.md §4.2 and the id table in
// §5.1. A test asserts every one of these; changing a spelling is a breaking
// change. `WASM.VALIDATE.RULE` is the transitional id for a validator rule
// that has not been mapped yet (DG-0 is incremental); it is the only id that
// may describe more than one rule.
namespace DiagnosticId {
inline constexpr std::string_view kDecodePreamble = "WASM.DECODE.PREAMBLE";
inline constexpr std::string_view kDecodeUnsupportedArtifact = "WASM.DECODE.UNSUPPORTED_ARTIFACT";
inline constexpr std::string_view kDecodeMalformed = "WASM.DECODE.MALFORMED";
inline constexpr std::string_view kDecodeUnsupported = "WASM.DECODE.UNSUPPORTED";
inline constexpr std::string_view kDecodeUnsupportedValueType = "WASM.DECODE.UNSUPPORTED_VALUE_TYPE";

inline constexpr std::string_view kValidateRule = "WASM.VALIDATE.RULE";
inline constexpr std::string_view kValidateUnknownType = "WASM.VALIDATE.UNKNOWN_TYPE";
inline constexpr std::string_view kValidateTypeNotFunction = "WASM.VALIDATE.TYPE_NOT_FUNCTION";
inline constexpr std::string_view kValidateUnknownFunction = "WASM.VALIDATE.UNKNOWN_FUNCTION";
inline constexpr std::string_view kValidateUnknownTable = "WASM.VALIDATE.UNKNOWN_TABLE";
inline constexpr std::string_view kValidateUnknownMemory = "WASM.VALIDATE.UNKNOWN_MEMORY";
inline constexpr std::string_view kValidateUnknownGlobal = "WASM.VALIDATE.UNKNOWN_GLOBAL";
inline constexpr std::string_view kValidateUnknownTag = "WASM.VALIDATE.UNKNOWN_TAG";
inline constexpr std::string_view kValidateUnknownLocal = "WASM.VALIDATE.UNKNOWN_LOCAL";
inline constexpr std::string_view kValidateUnknownElement = "WASM.VALIDATE.UNKNOWN_ELEMENT_SEGMENT";
inline constexpr std::string_view kValidateUnknownData = "WASM.VALIDATE.UNKNOWN_DATA_SEGMENT";

inline constexpr std::string_view kExecuteException = "WASM.EXECUTE.EXCEPTION";
inline constexpr std::string_view kExecuteLogInfo = "WASM.EXECUTE.LOG.INFO";
inline constexpr std::string_view kExecuteLogWarn = "WASM.EXECUTE.LOG.WARN";
inline constexpr std::string_view kExecuteLogError = "WASM.EXECUTE.LOG.ERROR";
inline constexpr std::string_view kExecuteLogPanic = "WASM.EXECUTE.LOG.PANIC";
} // namespace DiagnosticId

// `WASM.EXECUTE.TRAP.<NAME>` for a runtime trap kind; `TrapKind::Internal`
// maps to `WASM.EXECUTE.TRAP.INTERNAL`. The mapping is total and one-to-one.
std::string_view trapDiagnosticId(TrapKind kind) noexcept;
// `WASM.CONTINUATION.<NAME>` for a resume status.
std::string_view continuationDiagnosticId(ContinuationStatus status) noexcept;

// ── Sink and bounded ring ──────────────────────────────────────────────────

using DiagnosticSink = void (*)(void* userData, const Diagnostic& diagnostic) noexcept;

// The per-execution-state store. It lives in `ExecutionState`
// (WasmAgent.hpp), so two agents sharing a host thread do not observe each
// other's records and a continuation can carry its own. The members are
// public because an embedder that already holds an `Agent` may configure it
// directly; the free functions below are the common case (they act on the
// current execution state).
struct DiagnosticState {
	// Small on purpose: this is a debugging history, not a log.
	static constexpr std::uint32_t kDefaultCapacity = 32;

	DiagnosticSink sink = nullptr;
	void* userData = nullptr;
	std::uint32_t capacity = kDefaultCapacity;
	std::uint32_t sequence = 0;
	std::vector<Diagnostic> records;

	void setSink(DiagnosticSink newSink, void* newUserData) noexcept
	{
		sink = newSink;
		userData = newUserData;
	}

	// Changing the capacity keeps the newest records and drops the rest. A
	// capacity of zero disables the ring (a sink, if installed, still runs).
	void setCapacity(std::uint32_t newCapacity) noexcept;

	// Copies `diagnostic` into the ring (assigning `sequence`) and then calls
	// the sink. Never throws: a failed allocation simply loses the record.
	void emit(Diagnostic diagnostic) noexcept;

	std::span<const Diagnostic> recent() noexcept { return records; }
	void clear() noexcept { records.clear(); }
};

// Installs the sink on the current execution state. `nullptr` restores the
// default (no callback).
void setDiagnosticSink(DiagnosticSink sink, void* userData) noexcept;
// The records of the current execution state, oldest first. The span is
// invalidated by the next emission or by `clearDiagnostics()`.
std::span<const Diagnostic> recentDiagnostics() noexcept;
void clearDiagnostics() noexcept;
// 0 disables the ring for the current execution state.
void setDiagnosticRingCapacity(std::uint32_t capacity) noexcept;
// The single entry point every boundary uses. Assigning `sequence`, trimming
// the ring and calling the sink are all `DiagnosticState`'s job.
void emitDiagnostic(Diagnostic diagnostic) noexcept;

// ── Forms ──────────────────────────────────────────────────────────────────
//
// Text is the default, human-readable form:
//
//   error WASM.VALIDATE.UNKNOWN_TYPE: unknown type 7
//     at function 3 ($decode) instruction 42 (module byte 0x1a3)
//     hint: docs/VALIDATION.md#unknown-type
//
// JSON is schema v1, one object per record, emitted by the harness and by an
// opt-in embedder sink. The serializer uses the project's JSON library,
// nlohmann_json, behind this header (the include lives in the .cpp), so a
// consumer of the record does not pay for it and the output is real JSON
// rather than a second escape routine.
std::string formatDiagnostic(const Diagnostic& diagnostic);
std::string diagnosticToJson(const Diagnostic& diagnostic);
std::string diagnosticsToJson(std::span<const Diagnostic> diagnostics);

// The schema string a consumer may key on.
inline constexpr std::string_view kDiagnosticJsonSchema = "wasmjit.diagnostic.v1";

// ── Boundary helpers used by the core ──────────────────────────────────────

// Classifies and records a load-time failure that crossed `Module::fromFile`.
// The id is `WASM.DECODE.MALFORMED` for a "malformed" message and
// `WASM.DECODE.UNSUPPORTED` otherwise; per-site ids are DG-0 work.
void emitDecodeFailure(std::string_view message) noexcept;
// Records an uncaught exception at the point the host takes it from the
// pending slot (`wasm_exn_take_pending`).
void emitExceptionDiagnostic(std::int32_t tagId, std::uint32_t payloadBytes) noexcept;

// ── Guest log imports (unit-test facility) ─────────────────────────────────
//
// Four native imports a Wasm module may import from a module named by the
// caller (normally `env`). Each takes a pointer and a byte length into the
// module's memory 0 and reads UTF-8 — the same shape AssemblyScript's
// `@external("env", "info") declare function info(ptr: usize, len: i32)`
// produces. The bytes are copied into the record; a range outside linear
// memory yields a deterministic placeholder instead of reading anything.
//
//   info   → Note    WASM.EXECUTE.LOG.INFO
//   warn   → Warning WASM.EXECUTE.LOG.WARN
//   error  → Error   WASM.EXECUTE.LOG.ERROR
//   panic  → Fatal   WASM.EXECUTE.LOG.PANIC, then a trap (TrapKind::Internal)
//                    carrying the same text
//
// `panic` is what a unit test uses to fail loudly from inside the guest: the
// record makes the reason queryable and the trap fails the call. The bodies
// are ordinary native imports and must be registered through
// `WASM::nativeImport` (the trampoline), exactly like any host import.
//
// A message longer than `kMaxDiagnosticMessageBytes` is truncated, so a hostile
// module cannot make the host allocate a size it chose (docs/SAFETY.md §2.2).
inline constexpr std::uint32_t kMaxDiagnosticMessageBytes = 4096;

struct VMContext;
class ImportRegistrar;

// The bodies are ordinary host functions. They must not be `noexcept`:
// `panic` reports through `WASM::trap`, which throws to the `nativeImport`
// trampoline, and a `noexcept` body would terminate the process instead.
void diagnosticInfo(VMContext* context, std::int32_t pointer, std::int32_t length);
void diagnosticWarn(VMContext* context, std::int32_t pointer, std::int32_t length);
void diagnosticError(VMContext* context, std::int32_t pointer, std::int32_t length);
void diagnosticPanic(VMContext* context, std::int32_t pointer, std::int32_t length);

// Registers `info`, `warn`, `error` and `panic` under `moduleName`.
void registerDiagnosticImports(ImportRegistrar& registrar, std::string_view moduleName);
// The opt-in shortcut: registers them under `env`.
inline void registerToEnv(ImportRegistrar& registrar)
{
	registerDiagnosticImports(registrar, "env");
}

} // namespace WASM

#endif // WASMDIAGNOSTICS_HPP

