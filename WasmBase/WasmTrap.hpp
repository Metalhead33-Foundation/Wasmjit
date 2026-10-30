#ifndef WASMTRAP_HPP
#define WASMTRAP_HPP

#include "WasmApi.hpp"

/*!
  \file WasmTrap.hpp
  \brief The trap taxonomy and the pending-trap protocol.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstddef>
#include <cstdint>
#include <exception>

namespace WASM {

//! \par Wasm traps
//!
//! A trap is a *report*, not a non-local jump. The engine's own frames — the
//! generated code plus the native helpers it calls — carry no unwind tables, so
//! a C++ exception cannot leave a helper: it would have to unwind through a
//! generated frame. Instead:
//!
//!   * generated code and helpers call `reportTrap()` (or, from generated code,
//!     `wasm_trap_report()`) and *return a benign value*;
//!   * every caller checks the pending slot and returns in turn;
//!   * the host boundary (`callCallable`, `callStartFunction`, the test harness)
//!     converts a pending trap into a `WASM::Trap` C++ exception **after** the
//!     generated frames have returned.
//!
//! The consequence is that ordinary C++ lifetime rules hold everywhere: a helper
//! may hold `std::string`, `std::vector` or `std::unique_ptr` across a trap point
//! and its destructor runs on the early return. Nothing here requires an unwind
//! table in generated code, and there is no `setjmp`/`longjmp` anywhere.
//!
//! See docs/TRAP_CLEANUP.md §8.5/§9.7 for the design and §10 for the backend
//! study that selected it.

//! The *kind* of a trap: the spec's trap taxonomy. The kind is what an embedder or
//! a test compares — "did it trap for the right reason?" — while the free-text
//! message is only for the log. `trapKindName` returns the spec's wording for a
//! kind, which is exactly what the spec test suite asserts on ("null reference",
//! "out of bounds memory access", …), so an `assert_trap` expectation can be
//! checked against the kind instead of against the log text.
//!
//! See docs/SAFETY.md §2.4: a trap is Wasm semantics. An engine problem (a
//! compile-time limitation, a bad index that validation should have rejected) is
//! *not* a trap and must not be reported as one.

enum class TrapKind : uint8_t {
	Unreachable,                 //!< "unreachable"
	NullReference,               //!< "null reference"
	NullFunctionReference,       //!< "null function reference"
	NullStructureReference,      //!< "null structure reference"
	NullArrayReference,          //!< "null array reference"
	NullI31Reference,            //!< "null i31 reference"
	IntegerDivideByZero,         //!< "integer divide by zero"
	IntegerOverflow,             //!< "integer overflow"
	InvalidConversionToInteger,  //!< "invalid conversion to integer"
	InvalidString,               //!< "invalid string" (stringref encoding failure)
	OutOfBoundsMemoryAccess,     //!< "out of bounds memory access"
	OutOfBoundsTableAccess,      //!< "out of bounds table access"
	OutOfBoundsArrayAccess,      //!< "out of bounds array access"
	UnalignedAtomicAccess,       //!< "unaligned atomic"
	UninitializedElement,        //!< "uninitialized element"
	UndefinedElement,            //!< "undefined element"
	IndirectCallTypeMismatch,    //!< "indirect call type mismatch"
	CastFailure,                 //!< "cast failure"
	OutOfMemory,                 //!< "out of memory"
	CallStackExhausted,          //!< "call stack exhausted"
	//! Not a spec trap: a defensive check inside the engine, or a lowering that
	//! cannot run. The detail text is the only description.
	Internal, //!< An engine-internal trap; the detail text describes it.
};

//! The spec's text for a trap kind ("unreachable", "null reference", …). For
//! TrapKind::Internal it returns "internal error"; use the message for detail.
/** \brief Returns the specification spelling for a trap kind. \param kind Trap classification. \return Static null-terminated trap name. */
const char* trapKindName(TrapKind kind);

//! \par The pending-trap slot
//!
//! The slot belongs to the current execution state (`WasmAgent.hpp`): each agent
//! has its own pending trap, so two host threads — and two agents sharing one
//! host thread — cannot observe each other's traps. Generated code obtains its
//! address once per function through `wasm_trap_slot()` and reads `pending`
//! inline from then on.
//!
//! The cleanup stack and the two depth counters live in the same
//! `ExecutionState`, so they move with the agent rather than with the host
//! thread (TRAP-6, docs/TRAP_CLEANUP.md §12.1).
//!
//! The message is *stored in the slot*, not referenced: a native import that
//! reports `std::exception::what()` owns that text only until its frame returns,
//! and the boundary may read the trap much later.
inline constexpr std::size_t kTrapMessageCapacity = 192;

//! Absence marker for the compile-time trap site (generated code, a helper with
//! no site, or a host raise). Shared with `DiagnosticLocation`.
inline constexpr std::uint32_t kNoTrapIndex = 0xFFFFFFFFu;

//! The pending-trap slot: kind, message and the compile-time site (DG-5).
struct TrapSlot {
	int  pending = 0;                      //!< 0 = no trap, non-zero = pending
	int  kind = static_cast<int>(TrapKind::Internal); //!< TrapKind as an int (the C ABI)
	//! The trap site the backend knew at compile time (DG-5). Helpers and host
	//! raises leave `kNoTrapIndex`/null. `functionName` is interned by the
	//! instance, so it outlives the generated code that embeds the pointer.
	std::uint32_t functionIndex = kNoTrapIndex;
	std::uint32_t instructionOffset = kNoTrapIndex; //!< byte offset in the function body, or kNoTrapIndex
	const char*   functionName = nullptr; //!< interned name, or null when unknown
	char message[kTrapMessageCapacity] = {}; //!< NUL-terminated, truncated
};

//! The calling thread's slot. Never null; the returned object has static storage.
/** \brief C ABI access to the current execution state's trap slot. \return Non-null pointer to the current slot. */
extern "C" TrapSlot* wasm_trap_slot() noexcept;

//! Records `kind`/`detail` for the current call chain and returns; the caller
//! must return a benign value so its frame unwinds normally. The *first* trap
//! wins: a second report before the slot is cleared is ignored, so a failure in
//! trap-time cleanup cannot overwrite the original reason. Also resets the
//! tail-call trampoline state and runs any deferred cleanup thunks.
//!
//! `detail` is copied into the slot (truncated to `kTrapMessageCapacity`), so the
//! caller keeps ownership of its own text.
/** \brief Records the first pending trap and runs deferred cleanup. \param kind Trap classification. \param detail Optional diagnostic detail copied into the slot. */
void reportTrap(TrapKind kind, const char* detail = nullptr) noexcept;

//! The compile-time-site form used by generated code: additionally records the
//! module-local function index, the instruction byte offset in the body and the
//! interned function name (or kNoTrapIndex/nullptr when unknown). The site is
//! only stored when this report wins the first-trap race.
/** \brief Records a trap together with its generated-code source location. \param kind Trap classification. \param detail Optional message copied into the slot. \param functionIndex Module-local function index. \param instructionOffset Byte offset within the function body. \param functionName Interned function name, or nullptr when unavailable. */
void reportTrapAt(TrapKind kind, const char* detail,
		std::uint32_t functionIndex, std::uint32_t instructionOffset,
		const char* functionName) noexcept;

//! Fills the compile-time site of a trap that a *native helper* reported without
//! one. Generated code calls this on its pending-trap path, so the diagnostic and
//! the host exception name the function even when the failure happened inside a
//! helper (docs/proposals/EXTENDED_NAME_SECTION.md ENS7). The first site wins: a
//! callee that already annotated, or a dispatcher-emitted `reportTrapAt`, is left
//! untouched.
/** \brief Attaches a generated-code location to a pending helper-reported trap. \param functionIndex Module-local function index. \param instructionOffset Byte offset within the function body. \param functionName Interned function name, or nullptr when unavailable. */
void annotateTrapSite(std::uint32_t functionIndex,
		std::uint32_t instructionOffset, const char* functionName) noexcept;

//! C-callable `reportTrap`: the kind travels as an int so the calling convention
//! does not depend on how a scoped enum is passed.
/** \brief C ABI trap-reporting entry point. \param kind TrapKind value represented as int. \param message Optional detail string copied into the pending slot. */
extern "C" void wasm_trap_report(int kind, const char* message);
//! C-callable `reportTrapAt`: carries the compile-time trap site for DG-5.
/** \brief C ABI trap-reporting entry point including a source location. \param kind TrapKind value represented as int. \param message Optional detail string. \param functionIndex Module-local function index. \param instructionOffset Function-body byte offset. \param functionName Interned function name or nullptr. */
extern "C" void wasm_trap_report_at(int kind, const char* message,
		std::uint32_t functionIndex, std::uint32_t instructionOffset,
		const char* functionName);
//! C-callable `annotateTrapSite` for a pending trap that a helper reported.
/** \brief C ABI annotation for a pending trap that lacks a source location. \param functionIndex Module-local function index. \param instructionOffset Function-body byte offset. \param functionName Interned function name or nullptr. */
extern "C" void wasm_trap_annotate(std::uint32_t functionIndex,
		std::uint32_t instructionOffset, const char* functionName) noexcept;

//! True while a trap is pending on the calling thread. Generated code reads
//! `TrapSlot::pending` directly; C++ code uses this.
/** \brief Tests whether a trap is pending on the current execution state. \return true when the trap slot is pending. */
bool trapPending() noexcept;

//! The pending trap's kind, or TrapKind::Internal when none is pending.
/** \brief Reads the pending trap kind. \return Pending kind, or Internal when no trap is pending. */
TrapKind pendingTrapKind() noexcept;
//! The pending trap's message, or nullptr when none is pending. The returned
//! pointer is the slot's own buffer; it stays valid until the slot is cleared.
/** \brief Reads the pending trap message. \return Slot-owned message pointer, or nullptr when none is pending. */
const char* pendingTrapMessage() noexcept;

//! Clears the slot. The host boundary takes the trap before throwing it, so the
//! next call starts clean.
/** \brief Clears the current execution state's pending trap. */
void clearPendingTrap() noexcept;

//! A snapshot taken by the host boundary.
/** \brief Snapshot of a pending trap taken at a host boundary. */
struct TrapInfo {
	bool        trapped = false; //!< true when a trap was pending
	TrapKind    kind = TrapKind::Internal; //!< the pending kind, or Internal when none
	const char* message = nullptr; //!< slot message, or nullptr when none
	//! DG-5 site; kNoTrapIndex/null when the reporter had no compile-time site.
	std::uint32_t functionIndex = kNoTrapIndex;
	std::uint32_t instructionOffset = kNoTrapIndex; //!< byte offset, or kNoTrapIndex
	const char*   functionName = nullptr; //!< interned name, or null when unknown
};

//! Takes the pending trap and clears the slot. `trapped` is false (and kind/message
//! are the defaults) when nothing was pending.
/** \brief Takes and clears the pending trap slot. \return Trap snapshot, with defaults when no trap was pending. */
TrapInfo takePendingTrap() noexcept;

//! \brief The host-boundary exception
//!
//! Thrown only after generated code has returned to C++, so it never unwinds a
//! generated frame. The message is copied into a fixed buffer: the source may be
//! owned by a `ModuleInstance` that is destroyed while the exception unwinds (a
//! trapping start function does exactly that).
class Trap : public std::exception
{
public:
	static constexpr std::size_t kMessageCapacity = 240; //!< fixed buffer; source may outlive the throw

	//! Copies `message` into the fixed buffer (truncated) and records `kind`.
	/** \brief Constructs a host-boundary exception with a copied detail message. \param kind Trap classification. \param message Detail string copied into the exception's fixed buffer. */
	Trap(TrapKind kind, const char* message) noexcept;

	/** \brief Returns the trap classification. \return Stored TrapKind. */
	TrapKind kind() const noexcept { return kind_; } //!< the trap taxonomy value
	/** \brief Returns the copied trap detail. \return NUL-terminated exception-owned message. */
	const char* message() const noexcept { return message_; } //!< NUL-terminated detail text
	/** \brief Implements std::exception::what(). \return NUL-terminated exception-owned message. */
	const char* what() const noexcept override { return message_; } //!< std::exception spelling of message()

private:
	TrapKind kind_; //!< stored TrapKind
	char     message_[kMessageCapacity]; //!< owned copy of the detail text
};

//! If a trap is pending, takes it and throws `Trap`; otherwise does nothing.
//! POD-only and `noinline`: it is the single frame between the last generated
//! frame and the throw. Call it only at the host boundary, after generated code
//! has returned. Before throwing it records a `Diagnostic` through a `noexcept`
//! helper (docs/DIAGNOSTICS.md DG-2); that helper's own objects are destroyed
//! before the throw, so the throw itself still crosses a destructor-free frame.
//!
//! Nested calls are the exception: a host import (or the component runtime) that
//! re-enters Wasm must not throw into a generated frame. While `wasmCallDepth()`
//! is non-zero the trap is left pending and `checkTrap()` returns normally, so
//! the re-entering C++ code can observe `trapPending()` and return a benign
//! value; the generated caller then sees the pending slot.
/** \brief Throws a Trap at a safe host boundary when one is pending. \throws WASM::Trap when a pending trap can be transferred out of generated code. */
[[gnu::noinline]] void checkTrap();

//! Depth of host→Wasm calls in the current execution state. Zero means the
//! caller is outside the engine's generated frames, which is the only place
//! `Trap` may be thrown.
/** \brief Returns host-to-Wasm call depth in the current execution state. \return Current nested call depth. */
int wasmCallDepth() noexcept;

//! RAII increment/decrement of `wasmCallDepth()`. Every place that calls into
//! compiled code from C++ (the `callCallable` helpers, the start-function call,
//! the test harness) holds one for the duration of the call.
class CallDepthScope
{
public:
	/** \brief Increments the current execution state's Wasm call depth. */
	CallDepthScope() noexcept;
	/** \brief Decrements the call depth when the scope exits. */
	~CallDepthScope();
	CallDepthScope(const CallDepthScope&) = delete;
	CallDepthScope& operator=(const CallDepthScope&) = delete;
};

//! \par The native-import boundary
//!
//! A host import is called *from* generated code, so nothing may unwind through
//! it. `WasmBase/WasmNativeImport.hpp` wraps a typed host function in a
//! trampoline that catches exceptions and converts them into a pending trap; the
//! trampoline holds a `NativeImportScope` while the body runs. That is what lets
//! `WASM::trap()` (and `throw`) be used inside an import: the depth counter tells
//! it a C++ catch frame stands between the import and the generated caller.
/** \brief Returns the active native-import trampoline depth. \return Current native-import nesting depth. */
int nativeImportDepth() noexcept;

//! RAII increment/decrement of `nativeImportDepth()`. Held by the native-import
//! trampoline while the host body runs, so `WASM::trap()` may throw into the
//! trampoline's catch rather than through a generated frame.
class NativeImportScope
{
public:
	/** \brief Marks entry into a native-import exception-catching boundary. */
	NativeImportScope() noexcept;
	/** \brief Leaves the native-import boundary. */
	~NativeImportScope();
	NativeImportScope(const NativeImportScope&) = delete;
	NativeImportScope& operator=(const NativeImportScope&) = delete;
};

//! Records `kind` and throws `Trap` without returning. This is the host-facing
//! "raise" used by C++ APIs whose failure is a Wasm trap (`refDrop` underflow,
//! for example). It must *not* be called while generated code is on the stack:
//! the exception would have to unwind a generated frame. Helpers and native
//! imports use `reportTrap()` and return instead.
/** \brief Raises a host-side Trap immediately. Call only outside generated frames. \param kind Trap classification. \param detail Optional detail message. \throws WASM::Trap unconditionally. */
[[noreturn]] void trap(TrapKind kind, const char* detail = nullptr);

//! \brief Deferred cleanup (the optional hybrid)
//!
//! Under status-return traps every C++ frame unwinds normally, so RAII is the
//! primary cleanup mechanism and this stack is only for state that is *not*
//! owned by an object (an OS handle released by a C API, a host lock acquired by
//! an adapter, a C callback registration). Records are registered on the normal
//! path and dismissed explicitly:
//!
//!     auto token = WASM::defer(releaseThing, thing);   // non-allocating
//!     ... work that may trap ...
//!     WASM::dismissDeferred(token);
//!
//! `reportTrap()` runs the remaining records in LIFO order, once, before any
//! frame returns. A thunk must be `noexcept`, must not trap, and must not enter
//! Wasm; it may assume the frames that registered it are still alive.
/** \brief Token identifying one registered deferred-cleanup record. */
struct TrapDeferToken {
	std::uint32_t index = 0xFFFFFFFFu; //!< slot index, or all-ones when invalid
	bool valid() const noexcept { return index != 0xFFFFFFFFu; } //!< true when a live record
};

//! Registers `fn(arg)`. Returns an invalid token when the fixed-capacity stack is
//! full (the caller may then fall back to RAII or to its own cleanup).
/** \brief Registers a non-throwing cleanup callback for the next trap. \param fn Cleanup function; must not throw, trap, or re-enter Wasm. \param arg Opaque argument passed to fn. \return Valid token, or invalid token when the fixed-capacity stack is full. */
TrapDeferToken defer(void (*fn)(void*) noexcept, void* arg) noexcept;

//! Removes a record. Records are LIFO: dismissing a record that is not the most
//! recent one is a contract violation (asserted in debug builds) and returns
//! false; a no-op after a trap has drained the stack.
/** \brief Removes the most recently registered deferred cleanup. \param token Token returned by defer(). \return true when the record was removed; false for invalid/out-of-order tokens. */
bool dismissDeferred(TrapDeferToken token) noexcept;

//! Number of live records (diagnostics/tests).
/** \brief Returns the number of currently registered deferred cleanups. \return Live cleanup record count. */
std::uint32_t deferredCleanupCount() noexcept;

//! Runs every live record in LIFO order and clears the stack. Called by
//! `reportTrap`; exposed for tests.
/** \brief Runs and clears all registered cleanup callbacks in LIFO order. */
void runDeferredCleanup() noexcept;

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMTRAP_HPP
