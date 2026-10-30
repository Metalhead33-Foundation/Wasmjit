#ifndef WASMDIAGNOSTICS_HPP
#define WASMDIAGNOSTICS_HPP

#include "WasmApi.hpp"

/*!
  \file WasmDiagnostics.hpp
  \brief Structured diagnostics: records, phases, ids, sinks and serialization.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmContinuation.hpp"
#include "WasmTrap.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace WASM {

//! \par Structured diagnostics (docs/DIAGNOSTICS.md)
//!
//! Today the engine reports failures through several unrelated channels: a
//! named validation error, `TrapKind` plus a copied message, a pending
//! exception, a `ContinuationStatus`, and provider/scheduler state. Each is
//! useful on its own, but nothing correlates them into one record a test,
//! an embedder or a tool can consume without parsing prose.
//!
//! This header is that correlation layer, and it is backend-neutral: a record
//! carries facts the core owns (id, severity, phase, module-local location,
//! continuation context), never a backend type or host pointer. Backends feed
//! it through the same functions the core uses, so a diagnostic looks the same
//! whichever code generator produced the failing code.
//!
//! Design rules (docs/DIAGNOSTICS.md §4):
//!
//!   * `id` is the contract. Prose in `message` may improve without breaking an
//!     assertion; changing an id is a breaking change and needs a doc/test edit.
//!   * Emission collects at *boundaries* — the validator rule, the trap slot's
//!     conversion to a host exception, the pending-exception take, the four
//!     guest log imports. It is not threaded through every helper.
//!   * The ring is bounded and per execution state. A null sink with a zero
//!     capacity is the zero-cost configuration (no allocation on the path at
//!     all). The default capacity is deliberately small: diagnostics are a
//!     debugging surface, not a log file.
//!   * A sink is `noexcept` and must not throw; the engine cannot unwind a
//!     generated frame. It is called with the record itself *before* the record
//!     is appended to the ring, so a sink that re-enters the engine cannot
//!     invalidate what it is reading. `recentDiagnostics()` inside a sink does
//!     not yet include the record being delivered.
//!   * A record is self-contained: the message and the function name are owned
//!     strings, so a record copied into the ring or a sink outlives the module
//!     that produced it. The only `string_view`s are static literals (id,
//!     remediation, continuation state).

//! Severity is the severity of the *record*, not of the outcome. A `Note` from
//! `env.info` is still a successful call; a `Fatal` from `env.panic` is followed
//! by a trap. `Error` is a recoverable-as-a-trap failure (validation, a Wasm
//! trap, an uncaught exception). `Fatal` means the host facility itself declared
//! the run unusable.
enum class DiagnosticSeverity : std::uint8_t {
	Note, //!< Enum member `Note`.
	Warning, //!< Enum member `Warning`.
	Error, //!< Enum member `Error`.
	Fatal //!< Enum member `Fatal`.
};

//! The pipeline stage that produced the record. It is intentionally coarse: a
//! consumer switches on it, and a new stage is a deliberate addition.
enum class DiagnosticPhase : std::uint8_t {
	Decode, //!< DiagnosticPhase enumerator `Decode`.
	Validate, //!< DiagnosticPhase enumerator `Validate`.
	Compile, //!< DiagnosticPhase enumerator `Compile`.
	Instantiate, //!< DiagnosticPhase enumerator `Instantiate`.
	Execute, //!< DiagnosticPhase enumerator `Execute`.
	Continuation, //!< DiagnosticPhase enumerator `Continuation`.
	Component, //!< DiagnosticPhase enumerator `Component`.
};

//! "note" / "warning" / "error" / "fatal" — the text and JSON spelling.
const char* diagnosticSeverityName(DiagnosticSeverity severity) noexcept;
//! "decode" / "validate" / … / "component".
const char* diagnosticPhaseName(DiagnosticPhase phase) noexcept;
//! "native_stacks" / "host_threads".
const char* continuationProviderName(ContinuationProvider provider) noexcept;
//! "completed" / "suspended" / "trapped" / "exception" / "invalid".
const char* continuationStatusName(ContinuationStatus status) noexcept;

//! Absence markers. An all-unknown location is omitted from the text/JSON form
//! rather than serialized as 0 (a real function/offset is a valid 0).
inline constexpr std::uint32_t kNoDiagnosticIndex = 0xFFFFFFFFu;
//! The "no offset" marker that pairs with `kNoDiagnosticIndex`.
inline constexpr std::uint64_t kNoDiagnosticOffset = ~std::uint64_t{0};

//! Where the failure happened, in module terms. `functionIndex` is module-local
//! (imports first, then defined functions) and `instructionOffset` counts from
//! the start of the function body. `moduleOffset` is the byte offset in the
//! binary when the decode path knows it. `functionName` is the name-section
//! spelling without `$`; it is *owned* (not borrowed), so a record copied into
//! the ring or a sink stays valid after the module is destroyed.
struct DiagnosticLocation {
	//! The module-local function index, or `kNoDiagnosticIndex`.
	std::uint32_t functionIndex = kNoDiagnosticIndex;
	//! The instruction's byte offset in the body, or `kNoDiagnosticIndex`.
	std::uint32_t instructionOffset = kNoDiagnosticIndex;
	//! Byte offset in the artifact, or `kNoDiagnosticOffset`.
	std::uint64_t moduleOffset = kNoDiagnosticOffset;
	//! Name-section spelling without `$`; owned, so it outlives the module.
	std::string functionName;

	//! True when at least one part of the location is known.
	bool known() const noexcept
	{
		return functionIndex != kNoDiagnosticIndex
				|| instructionOffset != kNoDiagnosticIndex
				|| moduleOffset != kNoDiagnosticOffset;
	}
};

//! Continuation context for a trap, uncaught exception or scheduler failure
//! (docs/DIAGNOSTICS.md §4.3). It is populated when `state` is non-empty; that
//! is the single "present" marker, so a record that is not continuation-related
//! stays cheap and is serialized without a `continuation` object.
//!
//! `switchPeer` is developer-only: it is never serialized and never included in
//! the text form. Default JSON contains no host pointers, guest values or
//! `userData`.
struct DiagnosticContinuation {
	//! Which provider the continuation belongs to.
	ContinuationProvider provider = ContinuationProvider::NativeStacks;
	//! The handle's generation; a stale value means the slot was reused.
	std::uint64_t generation = 0;
	std::string_view state;   //!< "running", "suspended", "completed", "trapped", "exception"
	//! The thrown tag's id for an exception-carrying record, or -1.
	std::int32_t tagId = -1;
	//! Number of values the suspension carried.
	std::uint32_t valueCount = 0;
	//! Number of results the resume expects.
	std::uint32_t resultCount = 0;
	bool hasRoots = false; //!< True when the suspended frame holds GC roots to scan.
	const void* switchPeer = nullptr; //!< Developer-only peer state; never serialized.

	//! True when `state` is non-empty, i.e. this context is present at all.
	bool present() const noexcept { return !state.empty(); }
};

//! One correlated failure record: what happened (`id`, `severity`, `phase`,
//! `message`), where (`location`), in which execution context (`continuation`),
//! and when relative to the rest of the session (`sequence`, `timestamp`).
struct Diagnostic {
	std::string_view id;          //!< stable contract, e.g. "WASM.VALIDATE.UNKNOWN_TYPE"
	//! How serious the record is; a `Fatal` record is followed by a trap.
	DiagnosticSeverity severity = DiagnosticSeverity::Error;
	//! The phase that produced it (decode, validate, link, execute, …).
	DiagnosticPhase phase = DiagnosticPhase::Execute;
	std::string message;          //!< human-readable; not an assertion key
	//! Where in the module the failure happened; all-unknown when not known.
	DiagnosticLocation location;
	//! Execution-context detail for a trap, exception or scheduler failure.
	DiagnosticContinuation continuation;
	std::string_view remediation; //!< docs anchor or a short hint
	std::uint32_t sequence = 0;   //!< assigned by the emitting execution state
	//! Seconds since the epoch (`std::time(nullptr)`), assigned by `emit()`.
	//! Deliberately *not* converted to a date when serialized: JSON carries the
	//! raw number; only the pretty terminal form renders it via `ctime`.
	std::time_t timestamp = 0;
};

//! \par Stable identifiers
//!
//! The taxonomy is documented in docs/DIAGNOSTICS.md §4.2 and the id table in
//! §5.1. A test asserts every one of these; changing a spelling is a breaking
//! change. `WASM.VALIDATE.RULE` is the transitional id for a validator rule
//! that has not been mapped yet (DG-0 is incremental); it is the only id that
//! may describe more than one rule.
namespace DiagnosticId {
inline constexpr std::string_view kDecodePreamble = "WASM.DECODE.PREAMBLE"; //!< The `\0asm`/version/layer preamble is wrong.
inline constexpr std::string_view kDecodeUnsupportedArtifact = "WASM.DECODE.UNSUPPORTED_ARTIFACT"; //!< A recognised artifact kind this engine does not load.
inline constexpr std::string_view kDecodeMalformed = "WASM.DECODE.MALFORMED"; //!< The byte stream does not decode.
inline constexpr std::string_view kDecodeUnsupported = "WASM.DECODE.UNSUPPORTED"; //!< A construct the decoder does not implement.
inline constexpr std::string_view kDecodeUnsupportedValueType = "WASM.DECODE.UNSUPPORTED_VALUE_TYPE"; //!< A value type the engine does not model.

//! Backend plugin boundary (docs/BACKEND_PLUGIN_ABI.md BP-3/BP-4): failure to
//! load, an ABI mismatch, or a plugin's own diagnostic routed through the host
//! table.
inline constexpr std::string_view kBackendPlugin = "WASM.BACKEND.PLUGIN";

inline constexpr std::string_view kValidateRule = "WASM.VALIDATE.RULE"; //!< The transitional family for a rule not mapped to its own id yet.
inline constexpr std::string_view kValidateUnknownType = "WASM.VALIDATE.UNKNOWN_TYPE"; //!< A type index out of range.
inline constexpr std::string_view kValidateTypeNotFunction = "WASM.VALIDATE.TYPE_NOT_FUNCTION"; //!< An indirect call whose type is not a function type.
inline constexpr std::string_view kValidateUnknownFunction = "WASM.VALIDATE.UNKNOWN_FUNCTION"; //!< A function index out of range.
inline constexpr std::string_view kValidateUnknownTable = "WASM.VALIDATE.UNKNOWN_TABLE"; //!< A table index out of range.
inline constexpr std::string_view kValidateUnknownMemory = "WASM.VALIDATE.UNKNOWN_MEMORY"; //!< A memory index out of range.
inline constexpr std::string_view kValidateUnknownGlobal = "WASM.VALIDATE.UNKNOWN_GLOBAL"; //!< A global index out of range.
inline constexpr std::string_view kValidateUnknownTag = "WASM.VALIDATE.UNKNOWN_TAG"; //!< A tag index out of range.
inline constexpr std::string_view kValidateUnknownLocal = "WASM.VALIDATE.UNKNOWN_LOCAL"; //!< A local index out of range.
inline constexpr std::string_view kValidateUnknownElement = "WASM.VALIDATE.UNKNOWN_ELEMENT_SEGMENT"; //!< An element-segment index out of range.
inline constexpr std::string_view kValidateUnknownData = "WASM.VALIDATE.UNKNOWN_DATA_SEGMENT"; //!< A data-segment index out of range.

//! Rule families used by `validationRuleId()` for sites that do not pass an
//! explicit id. The message still names the precise rule; the id names the
//! stable family (docs/DIAGNOSTICS.md §4.2).
inline constexpr std::string_view kValidateUnknownLabel = "WASM.VALIDATE.UNKNOWN_LABEL"; //!< A label index deeper than the enclosing blocks.
inline constexpr std::string_view kValidateOperandStack = "WASM.VALIDATE.OPERAND_STACK"; //!< An operand-stack underflow or leftover values.
inline constexpr std::string_view kValidateTypeMismatch = "WASM.VALIDATE.TYPE_MISMATCH"; //!< An operand type the instruction does not accept.
inline constexpr std::string_view kValidateBlockType = "WASM.VALIDATE.BLOCK_TYPE"; //!< A block type that is neither a value type nor a type index.
inline constexpr std::string_view kValidateControlFlow = "WASM.VALIDATE.CONTROL_FLOW"; //!< A branch target outside the enclosing constructs.
inline constexpr std::string_view kValidateAlignment = "WASM.VALIDATE.ALIGNMENT"; //!< A `memarg` alignment wider than the access.
inline constexpr std::string_view kValidateOffsetRange = "WASM.VALIDATE.OFFSET_RANGE"; //!< A constant offset that overflows the address space.
inline constexpr std::string_view kValidateLimits = "WASM.VALIDATE.LIMITS"; //!< Limits out of order or over the engine's cap.
inline constexpr std::string_view kValidateImmutableTarget = "WASM.VALIDATE.IMMUTABLE_TARGET"; //!< A write to an immutable global.
inline constexpr std::string_view kValidateUninitializedLocal = "WASM.VALIDATE.UNINITIALIZED_LOCAL"; //!< A read of a local before it is set.
inline constexpr std::string_view kValidateUndeclaredRef = "WASM.VALIDATE.UNDECLARED_FUNCTION_REF"; //!< A `ref.func` naming a function the module never mentions.
inline constexpr std::string_view kValidateDuplicateExport = "WASM.VALIDATE.DUPLICATE_EXPORT"; //!< Two exports with the same name.
inline constexpr std::string_view kValidateSection = "WASM.VALIDATE.SECTION_CONSISTENCY"; //!< Section order, size or uniqueness violation.
inline constexpr std::string_view kValidateConstantTime = "WASM.VALIDATE.CONSTANT_TIME"; //!< A constant-time-profile violation.
inline constexpr std::string_view kValidateProfile = "WASM.VALIDATE.PROFILE"; //!< A construct the active profile excludes.
inline constexpr std::string_view kValidateDescriptor = "WASM.VALIDATE.DESCRIPTOR"; //!< A custom-descriptor clause mismatch.
inline constexpr std::string_view kValidateCast = "WASM.VALIDATE.CAST"; //!< A `ref.cast`/`ref.test` target mismatch.
inline constexpr std::string_view kValidateAtomicOrdering = "WASM.VALIDATE.ATOMIC_ORDERING"; //!< An invalid atomic-ordering immediate.
inline constexpr std::string_view kValidateAtomic = "WASM.VALIDATE.ATOMIC"; //!< An atomic access to a non-shared memory.
inline constexpr std::string_view kValidateContinuation = "WASM.VALIDATE.CONTINUATION"; //!< A continuation instruction/type mismatch.
inline constexpr std::string_view kValidateString = "WASM.VALIDATE.STRING"; //!< A stringref instruction/type mismatch.
inline constexpr std::string_view kValidateStruct = "WASM.VALIDATE.STRUCT"; //!< A struct field access that does not match the type.
inline constexpr std::string_view kValidateArray = "WASM.VALIDATE.ARRAY"; //!< An array instruction/type mismatch.
inline constexpr std::string_view kValidateTag = "WASM.VALIDATE.TAG"; //!< A tag signature mismatch.
inline constexpr std::string_view kValidateTable = "WASM.VALIDATE.TABLE"; //!< A table instruction/type mismatch.
inline constexpr std::string_view kValidateMemory = "WASM.VALIDATE.MEMORY"; //!< A memory instruction/type mismatch.
inline constexpr std::string_view kValidateGlobal = "WASM.VALIDATE.GLOBAL"; //!< A global instruction/type mismatch.
inline constexpr std::string_view kValidateLocal = "WASM.VALIDATE.LOCAL"; //!< A local instruction whose operand does not match the local.
inline constexpr std::string_view kValidateSelect = "WASM.VALIDATE.SELECT"; //!< A `select` whose arms disagree.
inline constexpr std::string_view kValidateShuffle = "WASM.VALIDATE.SHUFFLE"; //!< An `i8x16.shuffle` lane index above 31.
inline constexpr std::string_view kValidateTypeKind = "WASM.VALIDATE.TYPE_KIND"; //!< A type used with the wrong kind.

inline constexpr std::string_view kExecuteException = "WASM.EXECUTE.EXCEPTION"; //!< An uncaught Wasm exception reached the host boundary.
inline constexpr std::string_view kExecuteLogInfo = "WASM.EXECUTE.LOG.INFO"; //!< An `env.info` host-log record.
inline constexpr std::string_view kExecuteLogWarn = "WASM.EXECUTE.LOG.WARN"; //!< An `env.warn` host-log record.
inline constexpr std::string_view kExecuteLogError = "WASM.EXECUTE.LOG.ERROR"; //!< An `env.error` host-log record.
inline constexpr std::string_view kExecuteLogPanic = "WASM.EXECUTE.LOG.PANIC"; //!< An `env.panic` host-log record.

//! Continuation failures (DG-4). These are reasons, not the five resume
//! statuses: `WASM.CONTINUATION.INVALID` says the resume failed, while
//! `INVALID_GENERATION` says why. `continuationDiagnosticId()` maps the compact
//! status enum; these are the leaves emitted at the failure site.
inline constexpr std::string_view kContinuationNullHandle = "WASM.CONTINUATION.NULL_HANDLE";
inline constexpr std::string_view kContinuationInvalidHandle = "WASM.CONTINUATION.INVALID_HANDLE"; //!< The handle is not one this provider issued.
inline constexpr std::string_view kContinuationNullException = "WASM.CONTINUATION.NULL_EXCEPTION"; //!< A `resume_throw` carried no exception.
inline constexpr std::string_view kContinuationNullTarget = "WASM.CONTINUATION.NULL_TARGET"; //!< A call through a null continuation target.
inline constexpr std::string_view kContinuationCancelled = "WASM.CONTINUATION.CANCELLED"; //!< The continuation was cancelled before it resumed.
inline constexpr std::string_view kContinuationInvalidGeneration = "WASM.CONTINUATION.INVALID_GENERATION"; //!< The handle's generation is stale (the slot was reused).
inline constexpr std::string_view kContinuationNotResumable = "WASM.CONTINUATION.NOT_RESUMABLE"; //!< The continuation is completed or already running.
inline constexpr std::string_view kContinuationFreshThrow = "WASM.CONTINUATION.FRESH_THROW"; //!< A fresh `resume_throw` matched no handler clause.
inline constexpr std::string_view kContinuationCrossProvider = "WASM.CONTINUATION.CROSS_PROVIDER"; //!< The handle belongs to a different provider.
inline constexpr std::string_view kContinuationDroppedSuspended = "WASM.CONTINUATION.DROPPED_SUSPENDED"; //!< A suspended continuation was dropped.
inline constexpr std::string_view kContinuationSwitchOutside = "WASM.CONTINUATION.SWITCH_OUTSIDE"; //!< A `switch` outside a continuation body.
inline constexpr std::string_view kContinuationTargetCompleted = "WASM.CONTINUATION.TARGET_COMPLETED"; //!< The resume target had already completed.
inline constexpr std::string_view kContinuationSuspendOutside = "WASM.CONTINUATION.SUSPEND_OUTSIDE"; //!< A `suspend` outside a continuation body.
inline constexpr std::string_view kContinuationBudgetExhausted = "WASM.CONTINUATION.BUDGET_EXHAUSTED"; //!< The provider's budget refused the operation.
inline constexpr std::string_view kContinuationAllocationFailed = "WASM.CONTINUATION.ALLOCATION_FAILED"; //!< Allocating the continuation failed.
inline constexpr std::string_view kContinuationStartFailed = "WASM.CONTINUATION.START_FAILED"; //!< The continuation's start function failed.
inline constexpr std::string_view kContinuationStackAllocationFailed = "WASM.CONTINUATION.STACK_ALLOCATION_FAILED"; //!< Allocating the continuation's stack failed.
inline constexpr std::string_view kContinuationBindFailed = "WASM.CONTINUATION.BIND_FAILED"; //!< `cont.bind` could not bind the requested prefix.
inline constexpr std::string_view kContinuationUnhandledSuspension = "WASM.CONTINUATION.UNHANDLED_SUSPENSION"; //!< A suspension reached a boundary that cannot carry it.
inline constexpr std::string_view kContinuationSchedulerInvalidHandle = "WASM.CONTINUATION.SCHEDULER.INVALID_HANDLE"; //!< The scheduler was handed a task it does not own.
inline constexpr std::string_view kContinuationSchedulerRejected = "WASM.CONTINUATION.SCHEDULER.REJECTED"; //!< The scheduler refused to enqueue the task.
inline constexpr std::string_view kContinuationSchedulerTrapped = "WASM.CONTINUATION.SCHEDULER.TRAPPED"; //!< A scheduled task trapped.
inline constexpr std::string_view kContinuationSchedulerInvalid = "WASM.CONTINUATION.SCHEDULER.INVALID"; //!< The scheduler is in an unusable state.
} // namespace DiagnosticId

//! The diagnostic id of a runtime trap kind. The ids live under
//! `WASM.EXECUTE.TRAP`; `TrapKind::Internal` maps to
//! `WASM.EXECUTE.TRAP.INTERNAL`, and the mapping is total and one-to-one.
std::string_view trapDiagnosticId(TrapKind kind) noexcept;
//! The diagnostic id of a resume status (under `WASM.CONTINUATION`).
std::string_view continuationDiagnosticId(ContinuationStatus status) noexcept;
//! The rule family for a validator message (DG-0). The caller's explicit id
//! always wins; this is the default for the ~250 rule sites that still pass a
//! message only. Matching is ordered (docs/DIAGNOSTICS.md §4.2) and never
//! allocates. `WASM.VALIDATE.RULE` is only for a message no family matches.
std::string_view validationRuleId(std::string_view message) noexcept;

//! \par Sink and bounded ring

//! Type alias `DiagnosticSink` names the corresponding implementation/API type.
using DiagnosticSink = void (*)(void* userData, const Diagnostic& diagnostic) noexcept;

//! The per-execution-state store. It lives in `ExecutionState`
//! (WasmAgent.hpp), so two agents sharing a host thread do not observe each
//! other's records and a continuation can carry its own. The members are
//! public because an embedder that already holds an `Agent` may configure it
//! directly; the free functions below are the common case (they act on the
//! current execution state).
struct DiagnosticState {
	//! Small on purpose: this is a debugging history, not a log.
	static constexpr std::uint32_t kDefaultCapacity = 32;

	//! The installed sink, or null for none.
	DiagnosticSink sink = nullptr;
	//! The opaque pointer the sink receives alongside each record.
	void* userData = nullptr;
	//! Records the ring keeps; `0` disables the ring (a sink still runs).
	std::uint32_t capacity = kDefaultCapacity;
	//! Monotonic counter stamped onto each emitted record.
	std::uint32_t sequence = 0;
	//! The ring's records, oldest first.
	std::vector<Diagnostic> records;
	//! When true, one ANSI/emoji line is written to stderr per host-log record.
	//! The record id family is `WASM.EXECUTE.LOG.*`. Off by default: the core
	//! never writes to the terminal unless an embedder (or the test harness)
	//! opts in. This is deliberately *not* applied to traps/validation records:
	//! the spec suite produces thousands of expected ones.
	bool prettyPrintLogs = false;
	//! Session start for the JSON envelope. Set once when the execution state is
	//! first used and never changed by `clear()`, so `diagnosticsToJson` always
	//! has a stable "start date" for the collected records.
	std::time_t startedAt = std::time(nullptr);

	//! Installs `newSink`/`newUserData`; a null sink disables the callback.
	void setSink(DiagnosticSink newSink, void* newUserData) noexcept
	{
		sink = newSink;
		userData = newUserData;
	}

	//! Changing the capacity keeps the newest records and drops the rest. A
	//! capacity of zero disables the ring (a sink, if installed, still runs).
	void setCapacity(std::uint32_t newCapacity) noexcept;

	//! Copies `diagnostic` into the ring (assigning `sequence` and `timestamp`),
	//! optionally writes a pretty log line, then calls the sink. Never throws: a
	//! failed allocation simply loses the record.
	void emit(Diagnostic diagnostic) noexcept;

	//! The ring's records, oldest first; invalidated by the next emission.
	std::span<const Diagnostic> recent() noexcept { return records; }
	//! Drops every record of this ring.
	void clear() noexcept { records.clear(); }
};

//! Installs the sink on the current execution state. `nullptr` restores the
//! default (no callback).
void setDiagnosticSink(DiagnosticSink sink, void* userData) noexcept;
//! The currently installed sink/user data, so a temporary sink (a test, a
//! scoped capture) can restore what it replaced.
DiagnosticSink diagnosticSink() noexcept;
//! The opaque `userData` pointer installed alongside the sink.
void* diagnosticSinkUserData() noexcept;
//! The records of the current execution state, oldest first. The span is
//! invalidated by the next emission or by `clearDiagnostics()`.
std::span<const Diagnostic> recentDiagnostics() noexcept;
//! Drops every record of the current execution state.
void clearDiagnostics() noexcept;
//! 0 disables the ring for the current execution state.
void setDiagnosticRingCapacity(std::uint32_t capacity) noexcept;
//! Enables/disables the pretty stderr line for host-log records on the current
//! execution state (default off). The unit-test harness turns it on.
void setDiagnosticPrettyPrinting(bool enabled) noexcept;
//! True when the pretty host-log line is enabled on the current execution state.
bool diagnosticPrettyPrinting() noexcept;
//! The current execution state's session start (JSON envelope `startedAt`).
std::time_t diagnosticsStartedAt() noexcept;
//! The single entry point every boundary uses. Assigning `sequence`/`timestamp`,
//! trimming the ring and calling the sink are all `DiagnosticState`'s job.
void emitDiagnostic(Diagnostic diagnostic) noexcept;

//! \par Forms
//!
//! Text is the default, human-readable form:
//!
//!   error WASM.VALIDATE.UNKNOWN_TYPE: unknown type 7
//!     at function 3 ($decode) instruction 42 (module byte 0x1a3)
//!     hint: docs/VALIDATION.md\#unknown-type
//!
//! The *pretty* form is the terminal-facing one used by the host-log imports:
//! an emoji, then an ANSI-coloured (white/yellow/red/magenta) line containing
//! the ctime-rendered timestamp and the message, reset at end of line.
//!
//! JSON is schema v1. A single record is one object; a collected run is an
//! envelope `{schema, startedAt, records}`, where `startedAt` is the execution
//! state's session start (seconds since the epoch, not a formatted date).
//! Timestamps on records are numeric too — only the pretty form renders a date.
//! The serializer uses the project's JSON library, nlohmann_json, behind this
//! header (the include lives in the .cpp), so a consumer of the record does not
//! pay for it.
std::string formatDiagnostic(const Diagnostic& diagnostic);
//! `useColor` selects the ANSI escapes; the emoji and the date stay either way.
std::string formatDiagnosticPretty(const Diagnostic& diagnostic,
		bool useColor = true);
//! One record as a schema-v1 JSON object (nlohmann_json lives in the .cpp).
std::string diagnosticToJson(const Diagnostic& diagnostic);
//! `startedAt == 0` derives the envelope start from the first record (or now
//! when the span is empty), so a caller that does not track a session start
//! still gets a usable envelope. Pass `diagnosticsStartedAt()` for the exact
//! execution-state start.
std::string diagnosticsToJson(std::span<const Diagnostic> diagnostics,
		std::time_t startedAt = 0);

//! The schema string a consumer may key on.
inline constexpr std::string_view kDiagnosticJsonSchema = "wasmjit.diagnostic.v1";

//! \par Boundary helpers used by the core

//! Classifies and records a load-time failure that crossed `Module::fromFile`.
//! The id is `WASM.DECODE.MALFORMED` for a "malformed" message and
//! `WASM.DECODE.UNSUPPORTED` otherwise; per-site ids are DG-0 work.
void emitDecodeFailure(std::string_view message) noexcept;
//! Records an uncaught exception at the point the host takes it from the
//! pending slot (`wasm_exn_take_pending`).
void emitExceptionDiagnostic(std::int32_t tagId, std::uint32_t payloadBytes) noexcept;
//! Records a trap the host has just observed. The engine's own boundaries
//! (`checkTrap`, `trap`) call this; a harness that reads the pending slot
//! directly — the test backend and the diagnose tool do — must call it at its
//! own boundary so the record exists exactly once. The optional site is the
//! compile-time function/instruction the backend reported (DG-5); pass
//! `kNoDiagnosticIndex`/nullptr when unknown.
void emitTrapDiagnostic(TrapKind kind, const char* message,
		std::uint32_t functionIndex = kNoDiagnosticIndex,
		std::uint32_t instructionOffset = kNoDiagnosticIndex,
		const char* functionName = nullptr) noexcept;
//! Records a continuation/scheduler failure with the provider, generation,
//! state, tag, value/result counts and root presence the runtime already has
//! (DG-4). `continuation.state` is the "populated" marker; when it is empty the
//! record has no continuation object (a null or invalid handle, for example).
//! `switchPeer` is never serialized. Allocation failures are swallowed: the
//! original trap/status is the outcome that matters.
void emitContinuationDiagnostic(std::string_view id, DiagnosticSeverity severity,
		std::string message, DiagnosticContinuation continuation,
		std::string_view remediation = "docs/proposals/STACK_SWITCHING.md") noexcept;

//! \par Guest log imports (unit-test facility)
//!
//! Four native imports a Wasm module may import from a module named by the
//! caller (normally `env`). Each takes a pointer and a byte length into the
//! module's memory 0 and reads UTF-8 — the same shape AssemblyScript's
//! `@external("env", "info") declare function info(ptr: usize, len: i32)`
//! produces. The bytes are copied into the record; a range outside linear
//! memory yields a deterministic placeholder instead of reading anything.
//!
//!   info   → Note    WASM.EXECUTE.LOG.INFO
//!   warn   → Warning WASM.EXECUTE.LOG.WARN
//!   error  → Error   WASM.EXECUTE.LOG.ERROR
//!   panic  → Fatal   WASM.EXECUTE.LOG.PANIC, then a trap (TrapKind::Internal)
//!                    carrying the same text
//!
//! `panic` is what a unit test uses to fail loudly from inside the guest: the
//! record makes the reason queryable and the trap fails the call. The bodies
//! are ordinary native imports and must be registered through
//! `WASM::nativeImport` (the trampoline), exactly like any host import.
//!
//! A message longer than `kMaxDiagnosticMessageBytes` is truncated, so a hostile
//! module cannot make the host allocate a size it chose (docs/SAFETY.md §2.2).
inline constexpr std::uint32_t kMaxDiagnosticMessageBytes = 4096;

struct VMContext;
class ImportRegistrar;

//! The bodies are ordinary host functions. They must not be `noexcept`:
//! `panic` reports through `WASM::trap`, which throws to the `nativeImport`
//! trampoline, and a `noexcept` body would terminate the process instead.
void diagnosticInfo(VMContext* context, std::int32_t pointer, std::int32_t length);
//! `env.warn`: records a `Warning` (`WASM.EXECUTE.LOG.WARN`).
void diagnosticWarn(VMContext* context, std::int32_t pointer, std::int32_t length);
//! `env.error`: records an `Error` (`WASM.EXECUTE.LOG.ERROR`).
void diagnosticError(VMContext* context, std::int32_t pointer, std::int32_t length);
//! `env.panic`: records a `Fatal` and then traps with the same text.
void diagnosticPanic(VMContext* context, std::int32_t pointer, std::int32_t length);

//! Registers `info`, `warn`, `error` and `panic` under `moduleName`.
void registerDiagnosticImports(ImportRegistrar& registrar, std::string_view moduleName);
//! The opt-in shortcut: registers them under `env`.
inline void registerToEnv(ImportRegistrar& registrar)
{
	registerDiagnosticImports(registrar, "env");
}

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMDIAGNOSTICS_HPP

