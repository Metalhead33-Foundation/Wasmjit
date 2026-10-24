#ifndef LIBJITOPCODEDISPATCHER_HPP
#define LIBJITOPCODEDISPATCHER_HPP

#include "../WasmBase/WasmModuleInstance.hpp"
#include "../WasmBase/WasmOpcodeDispatcher.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "LibJitTypeTranslation.hpp"
#include <jit/jit.h>
#include <span>
#include <utility>
#include <vector>

namespace LibJIT {

struct ControlBlock {
	enum Kind { Block, Loop, If, TryTable };
	Kind kind;

	// For Block/If: a forward label at the end (bound when we see 'end').
	// For Loop: a backward label at the start (bound when we see 'loop').
	// A 'br N' instruction branches to controlStack[top - N].label.
	jit_label_t label;

	// For 'if': we also need a label for the 'else' branch, so that
	// the condition check can jump forward to it if false.
	jit_label_t elseLabel;

	// The value stack depth when this block was entered. Used to
	// validate and restore the stack at 'else' and 'end' boundaries.
	size_t stackDepth;

	// The types this block is expected to produce on exit.
	// A 'br' to this block must leave these types on the stack.
	std::vector<WASM::StorageType> resultTypes;
	std::vector<WASM::StorageType> paramTypes;
	std::vector<jit_value_t> resultSlots;
	std::vector<jit_value_t> paramSlots;
	bool hasElse = false;
	// try_table only: the decoded catch clauses. They are not a runtime
	// construct — throw sites dispatch to them statically (EXCEPTIONS.md
	// EXD2). `labelIdx` is relative to this frame's *parent*.
	std::vector<WASM::CatchClause> catchClauses;
};

// Drives WASM::OpcodeDispatcher::readCode for LibJIT: decodes the bytecode
// stream and emits jit_insn_* into the current jit_function_t. Holds the
// transient compilation state (locals, stacks, module context) that
// ModuleCompiler used to pass through a monolithic dispatchOpcode.
class OpcodeDispatcher : public WASM::OpcodeDispatcher
{
private:
	jit_context_t context;
	jit_function_t function;
	LibJitTypeTranslator& typeTranslator;
	WASM::ModuleInstance& instance;
	WASM::ModuleInstanceInternals& internals;
	const WASM::Module& module;
	const WASM::FuncType& currentFunc;
	uint32_t importedFuncCount;
	// Global function index of the body being compiled (imports first), and its
	// diagnostic label (`function 3 ($name)`). Emitted traps carry the label so
	// a runtime failure names the function, not just the trap kind
	// (docs/proposals/EXTENDED_NAME_SECTION.md ENS-5).
	uint32_t functionIndex;
	std::string functionLabel;
	std::vector<jit_value_t>& locals;
	std::vector<jit_value_t>& valueStack;
	std::vector<ControlBlock>& controlStack;

	void pushValue(jit_value_t v);
	// Push a reference whose ownership the stack takes over (no clone).
	void pushOwnedValue(jit_value_t v);
	// Release the operand stack's ownership of a popped reference.
	void emitDropValue(jit_value_t v);
	// Release every reference-typed non-parameter local (function exit).
	void emitDropRefLocals();
	jit_value_t popValue();
	// Emits the function's epilogue (a `return`). Does nothing in unreachable
	// code or when the model stack is short (dead tail). With `consume` false
	// the return operands are left on the model stack (for a conditional
	// branch to the function's implicit block, whose fallthrough needs them).
	void emitFunctionReturn(bool consume = true);
	// Emits a native tail call (LibJIT `JIT_CALL_TAIL`) when the target is this
	// very function — the only case this LibJIT build lowers reliably (see
	// docs/TAILCALLS.md §6). Returns false when the caller must fall back to
	// `call` + `return`.
	bool emitDirectTailCall(WASM::FuncIdx funcIdx);
	// True when `calleeSig` has exactly this function's parameter and result
	// types. Only then can the trampoline driver (built for this function's
	// signature) re-dispatch to the callee. See docs/TAILCALLS.md TC-2.
	bool signatureIsUniform(const WASM::FuncType& calleeSig) const;
	// Publishes {target, args, pending} to the global trampoline state and
	// returns a dummy value; the driver re-dispatches. Callers must have
	// verified signatureIsUniform() and popped `args` already.
	void emitTailCallViaPending(jit_value_t targetCallablePtr, const std::vector<jit_value_t>& args);
	// Emits `return` of a zero value of this function's result type.
	void emitDummyReturn();
	// Emits the jump for a branch target; a target past the control stack is
	// the function's implicit block (i.e. a return).
	void emitBranchToLabel(WASM::LabelIdx arg);
	// Marks the rest of the current block unreachable (after an unconditional
	// transfer). In unreachable code the Wasm operand stack is polymorphic, so
	// popValue() yields placeholders instead of underflowing.
	void markUnreachable();
	bool unreachableCode = false;

	// Branch hints for the function being compiled, ordered by byte offset
	// (already rebased onto the opcode stream). Empty when the module carries
	// no `metadata.code.branch_hint` section, in which case codegen is
	// byte-identical to the hint-less path.
	std::vector<WASM::BranchHint> branchHints;
	// Returns the hint attached to the instruction currently being dispatched,
	// or nullptr if there is none. Purely an optimisation input; never changes
	// results.
	const WASM::BranchHint* currentBranchHint() const;
	jit_value_t zeroConstantForType(jit_type_t t);

	// ── SIMD lowering (docs/SIMD_LIBJIT.md §6) ───────────────────────────────
	//
	// A `v128` operand is a struct-typed LibJIT value whose 16 bytes live in a
	// frame slot; helpers never see the value, only its *address* (§6.3), and
	// they write their result into a fresh slot. The mechanics were verified
	// against this LibJIT build before the design was written down:
	//   - `jit_insn_address_of` works on a struct parameter, on a struct local,
	//     and on `jit_insn_load` of either;
	//   - `jit_insn_store(local, value)` works for a struct local (so `local.set`
	//     of a `v128` is fine);
	//   - `jit_insn_store_relative` does **not** write a `jit_value_create`d
	//     aggregate, so `v128.const` must go through a helper, not inline stores.
	jit_type_t v128Type();
	// A fresh 16-byte slot for a `v128` result.
	jit_value_t newV128Slot();
	// Address of a global's 16 bytes in the VMContext's `globals` array. The
	// `v128` member sits at offset 0 of the `Value` union, so this is also the
	// pointer the load/store helpers accept (SIMD-L5).
	jit_value_t globalSlotAddress(WASM::GlobalIdx arg);
	// Calls a helper through a memoised signature (§6.4) and returns its result
	// (`void` for the out-slot shapes — the caller then pushes the slot).
	jit_value_t callSimdHelper(void* fptr, jit_type_t ret,
			std::span<const jit_type_t> params, std::span<const jit_value_t> args);
	// The common shape: leading operands, then the result slot's address.
	jit_value_t callSimdOutHelper(void* fptr, jit_value_t out,
			std::span<const jit_type_t> leadingTypes, std::span<const jit_value_t> leadingArgs);
	// The `v128.load*` / `v128.store*` shapes. The effective address is computed
	// and bounds-checked by `effectiveMemoryAddress` — the same path the scalar
	// loads take, trap included — so the helpers themselves never trap and need
	// no context (docs/SIMD_LIBJIT.md §6.3). Operand order is wasm's: for a lane
	// or store form the vector is on top, the address below it.
	void emitSimdLoad(const WASM::MemArg& ma, jit_nint width, void* helper);
	void emitSimdLoadLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper);
	void emitSimdStore(const WASM::MemArg& ma, jit_nint width, void* helper);
	void emitSimdStoreLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper);
	// The per-lane shapes: two vectors in and one out (used for the arithmetic
	// *and* for the mask-producing comparisons — the difference is entirely in
	// the helper), one vector in and one out, and the three-operand bitselect.
	void emitSimdBinaryHelper(void* helper);
	void emitSimdUnaryHelper(void* helper);
	void emitSimdTernaryHelper(void* helper);
	// A shift takes the vector plus a scalar count; a reduction yields a wasm i32
	// rather than a vector, so it has no out slot at all.
	void emitSimdShiftHelper(void* helper);
	void emitSimdReduceI32Helper(void* helper);
	// Signature cache: the SIMD ops share a handful of shapes, so memoise them
	// instead of creating (and leaking) one signature per instruction (§6.4).
	std::vector<std::pair<std::vector<jit_type_t>, jit_type_t>> simdSignatureCache;
	jit_type_t simdCachedType = nullptr;
	jit_value_t castRefValue(jit_value_t value, jit_type_t targetType);
	jit_value_t refAsVoidPtr(jit_value_t value);
	// Emits the reference-counting write barrier (`wasm_ref_store`): retain
	// `newRef`, store it at `slotAddr`, release the value it overwrote. Used
	// for every reference-typed slot store (docs/proposals/GC.md D15/RC-1).
	void emitRefStore(jit_value_t slotAddr, jit_value_t newRef);
	// `wasm_ref_clone` / `wasm_ref_drop` (retain / release), for slot stores
	// that address a LibJIT local rather than memory (RC-5).
	jit_value_t emitRefClone(jit_value_t ref);
	void emitRefDrop(jit_value_t ref);
	jit_value_t typedNullRef(jit_type_t refType);
	jit_value_t emitRefTypeTest(jit_value_t ref, const WASM::HeapType& heapType, bool nullable);
	jit_type_t tableElementJitType(WASM::TableIdx arg) const;
	jit_type_t jitRefTypeForHeapType(const WASM::HeapType& ht, bool nullable);
	jit_type_t structRefType(WASM::TypeIdx arg, bool nullable) const;
	jit_type_t arrayRefType(WASM::TypeIdx arg, bool nullable) const;
	WASM::CoreTypeView structTypeForIndex(WASM::TypeIdx arg) const;
	WASM::CoreTypeView arrayTypeForIndex(WASM::TypeIdx arg) const;
	jit_type_t arrayElementJitType(WASM::TypeIdx arg) const;
	jit_nint structFieldOffset(WASM::TypeIdx arg, uint32_t fieldIndex) const;
	jit_nint arrayLengthOffset(WASM::TypeIdx arg) const;
	jit_nint arrayDataOffset(WASM::TypeIdx arg) const;
	// `ref + dataOffset`, typed as a pointer to the array's element type.
	// Arrays store their elements inline (docs/proposals/GC.md D13/GC-9), so
	// there is no `data` field to load.
	jit_value_t arrayElementBase(jit_value_t arrayRef, WASM::TypeIdx arg);
	// Layout of a multi-result function's result tuple. A multi-result
	// signature carries a trailing `void*` result buffer instead of returning
	// an aggregate by value (see LibJitTypeTranslation.cpp); this struct type
	// describes that buffer.
	jit_type_t resultTupleType(const WASM::FuncType& wasm_func);
	// Writes the top `resultCount` values into `out`, the caller-provided
	// result buffer (a multi-result function's trailing parameter).
	void packReturnValues(jit_type_t tupleType, jit_value_t out, size_t resultCount);
	// Pushes a call's results. For a multi-result callee `outBuffer` is the
	// address of the caller's result tuple and `ret` is unused (the callee
	// returns void); for 0/1 results `outBuffer` is null.
	void pushCallResults(const WASM::FuncType& calleeSig, jit_value_t ret, jit_value_t outBuffer);
	// Allocates the caller-side result tuple in this function's frame and
	// returns its address, or null when `calleeSig` has 0/1 results.
	jit_value_t newResultBuffer(const WASM::FuncType& calleeSig);
	std::vector<WASM::StorageType> parameterTypesForBlockType(const WASM::BlockType& bt) const;
	std::vector<jit_value_t> createSlotsForTypes(const std::vector<WASM::StorageType>& types);
	void storeStackTopToSlots(const std::vector<jit_value_t>& slots);
	void restoreValuesFromSlots(const std::vector<jit_value_t>& slots);
	void resizeValueStack(size_t newSize);
	ControlBlock& branchTarget(WASM::LabelIdx arg);
	const std::vector<WASM::StorageType>& branchTypesForTarget(const ControlBlock& target) const;
	const std::vector<jit_value_t>& branchSlotsForTarget(const ControlBlock& target) const;
	void emitBranchToTarget(ControlBlock& target);
	WASM::TypeIdx functionTypeIndexForFunc(WASM::FuncIdx funcIdx) const;
	WASM::FuncType functionSignatureForType(WASM::TypeIdx typeIdx) const;
	// `cont $ft`: resolve the module-local function type index stored in the
	// canonical continuation type (STACK_SWITCHING.md SS-1).
	WASM::TypeIdx continuationFuncIndex(WASM::TypeIdx contTypeIdx) const;
	WASM::FuncType continuationSignature(WASM::TypeIdx contTypeIdx) const;
	jit_value_t callablePointerForFuncIndex(WASM::FuncIdx funcIdx);
	void dispatchCallThroughCallable(jit_value_t callablePtr, WASM::TypeIdx typeIdx);
	// Emit a trap of `kind` (docs/SAFETY.md §2.4). `detail` overrides the log
	// message when the kind's spec name is too coarse for the situation.
	void emitTrap(WASM::TrapKind kind);
	void emitTrap(WASM::TrapKind kind, const char* detail);
	// Emit a trap of `kind` when `pointer` is null (a null reference reached an
	// instruction that dereferences it). The kind names *what* was dereferenced:
	// NullReference, NullFunctionReference, NullStructureReference,
	// NullArrayReference.
	void emitTrapIfNull(WASM::TrapKind kind, jit_value_t pointer, const char* detail = nullptr);
	std::vector<WASM::StorageType> storageTypesForBlockType(const WASM::BlockType& bt) const;

	jit_value_t vmContextValue();
	jit_value_t checkedTableIndex(jit_value_t table, jit_value_t index, const char* opname,
			WASM::TrapKind kind = WASM::TrapKind::UndefinedElement);
	// Emits code yielding the LinearMemory* for a (constant) memory index.
	jit_value_t memoryPointerForIndex(WASM::MemIdx memidx);
	// The declared index type (i32/i64) of memory `memidx`, for the address
	// widening in effectiveMemoryAddress.
	WASM::MemoryIndexType memoryIndexTypeForIndex(WASM::MemIdx memidx) const;
	// True when table `tableidx` declares a 64-bit index (flag bit 2).
	bool tableIs64Bit(WASM::TableIdx tableidx) const;

	// Emits code yielding the TableInstance* for a (constant) table index.
	jit_value_t tablePointerForIndex(WASM::TableIdx tableidx);
	// Emits a runtime check that `callablePtr`'s canonical type id matches the
	// module-local type index `typeIdx` (id fast path, registry fallback).
	void emitCallableTypeCheck(jit_value_t callablePtr, WASM::TypeIdx typeIdx);
	// A lowering the backend cannot perform (in practice: an unmodelled value
	// type, docs/NULL_JIT_VALUE_DIAGNOSIS.md). `notImplemented()` throws, which
	// surfaces as a clean instantiation failure; the LibJIT build region in
	// ModuleCompiler::compileFunctions is RAII-guarded (docs/SAFETY.md).
	[[noreturn]] void notImplemented(const char* name);

	jit_value_t effectiveMemoryAddress(const WASM::MemArg& ma, jit_nint accessSize);

	// ── 0xFE atomics (docs/proposals/THREADS.md TH-1) ──────────────────────
	// The shared helpers in WasmBase (wasm_atomic.hpp) perform the access with
	// `__atomic_*` at seq_cst and trap on a misaligned effective address; these
	// emit the call and adapt the value types. `effectiveMemoryAddress` has
	// already bounds-checked the address (and trapped on out-of-bounds).
	jit_value_t emitAtomicLoad(const WASM::MemArg& ma, uint32_t width, jit_type_t resultType);
	void        emitAtomicStore(const WASM::MemArg& ma, uint32_t width);
	jit_value_t emitAtomicRmw(const WASM::MemArg& ma, uint32_t width, uint32_t op,
							  jit_type_t resultType);
	jit_value_t emitAtomicCmpxchg(const WASM::MemArg& ma, uint32_t width,
							  jit_type_t resultType);
	jit_value_t emitAtomicLoadAddress(jit_value_t address, uint32_t width,
									  uint8_t ordering, jit_type_t resultType);
	void        emitAtomicStoreRaw(jit_value_t address, jit_value_t value,
								   uint32_t width, uint8_t ordering);
	jit_value_t emitAtomicRmwRaw(jit_value_t address, jit_value_t value,
								 uint32_t width, uint32_t op, uint8_t ordering,
								 jit_type_t resultType);
	jit_value_t emitAtomicCmpxchgRaw(jit_value_t address, jit_value_t expected,
									 jit_value_t replacement, uint32_t width,
									 uint8_t ordering, jit_type_t resultType);
	void        emitAtomicStoreAddress(jit_value_t address, uint32_t width,
									   uint8_t ordering);
	jit_value_t emitAtomicRmwAddress(jit_value_t address, uint32_t width,
									 uint32_t op, uint8_t ordering,
									 jit_type_t resultType);
	jit_value_t emitAtomicCmpxchgAddress(jit_value_t address, uint32_t width,
										 uint8_t ordering, jit_type_t resultType);
	jit_value_t tableAtomicSlotAddress(WASM::TableIdx tableIdx);
	jit_value_t structAtomicFieldAddress(WASM::TypeIdx typeIdx, uint32_t fieldIdx);
	jit_value_t arrayAtomicElementAddress(WASM::TypeIdx typeIdx);
	jit_value_t callNativeHelper(void* fptr, jit_type_t resultType,
								 std::vector<jit_type_t> paramTypes,
								 std::vector<jit_value_t> args);
	jit_value_t atomicUintConstant(uint32_t value);

	// ── Exceptions (docs/proposals/EXCEPTIONS.md EX-3/EX-4) ────────────────
	// Handlers are resolved statically: `throw` knows its tag at compile time,
	// `throw_ref` emits a comparison chain. With no handler in this function
	// the exception becomes *pending* and the function returns zeroes; every
	// call site checks the pending slot (EXD1).
	void pushBlockFrame(ControlBlock::Kind kind, const WASM::BlockType& arg,
						std::vector<WASM::CatchClause>&& catches);
	WASM::FuncType tagPayloadType(WASM::TagIdx tagIdx) const;
	WASM::TagId tagIdForClause(const WASM::CatchClause& clause) const;
	jit_value_t createException(WASM::TagId tagId, const WASM::FuncType& payload);
	void storeExceptionPayload(jit_value_t exnPayload, const WASM::FuncType& payload,
							   const std::vector<jit_value_t>& values);
	std::vector<jit_value_t> loadExceptionPayload(jit_value_t exnPayload,
												  const WASM::FuncType& payload);
	void emitThrowToTarget(jit_value_t exn, std::vector<jit_value_t> values,
						   ControlBlock& target, bool carryExn);
	// Emits the static dispatch chain for `exn` over the enclosing try_tables;
	// when no clause matches, sets the pending slot and returns zeroes. All
	// emitted paths leave the current block.
	void emitExceptionPropagation(jit_value_t exn);
	jit_value_t emitPendingException();
	// Used after every call: dispatches a pending exception locally or
	// propagates it, leaving the continuation reachable when none is pending.
	void emitPendingExceptionCheck();
	// Returns zeroes of the current function's result types (the ABI still
	// requires a well-formed return when an exception propagates).
	void emitReturnZeroes();

	// ── Trap status (docs/TRAP_CLEANUP.md §8.5) ────────────────────────────
	// A trap is *reported*, not thrown or jumped: generated code calls
	// `wasm_trap_report` and branches to this function's error epilogue, which
	// returns zeroes. Every call that can report is followed by a pending check.
	// No unwind table and no `longjmp` is involved, so a helper may hold a
	// `std::string` across the trap point.
	void emitPendingTrapCheck();
	void emitReportTrap(WASM::TrapKind kind, const char* detail);
	void requestTrapEpilogue();
	jit_label_t trapEpilogueLabel = jit_label_undefined;
	bool trapEpilogueUsed = false;
	// The calling thread's trap slot, obtained once per function in the entry
	// block (`wasm_trap_slot()`); every check loads `pending` through it.
	jit_value_t trapSlot = nullptr;
	// The same for the pending Wasm exception (`wasm_exn_slot()`): a `throw`
	// leaves a pointer there, and every call site checks it.
	jit_value_t exceptionSlot = nullptr;
	// Set while the epilogue itself emits helper calls: a failure there must not
	// branch back into the epilogue.
	bool suppressTrapChecks = false;
	// True when the active profile excludes the `N` marker: every scalar float
	// result is canonicalised at an observability point (return, store,
	// reinterpret, global.set). See docs/proposals/PROFILES.md PR-3.
	const bool deterministicFloats;

	WASM::GlobalType globalTypeForIndex(WASM::GlobalIdx idx) const;

	// Deterministic-profile float canonicalisation (PROFILES.md PR-3): returns
	// `value` unchanged unless `deterministicFloats` is set and the value is a
	// NaN, in which case a canonical NaN of the same width is returned.
	jit_value_t canonicalizeFloat(jit_value_t value, bool isDouble);

public:
	OpcodeDispatcher(jit_context_t context, jit_function_t function,
					 LibJitTypeTranslator& typeTranslator,
					 WASM::ModuleInstance& instance,
					 WASM::ModuleInstanceInternals& internals,
					 const WASM::Module& module, const WASM::FuncType& currentFunc,
					 uint32_t importedFuncCount, uint32_t functionIndex,
					 std::vector<WASM::BranchHint> branchHints,
					 std::vector<jit_value_t>& locals,
					 std::vector<jit_value_t>& valueStack,
					 std::vector<ControlBlock>& controlStack,
				 WASM::ProfileConfig profileConfig);

	// Call after readCode() to emit return from the implicit function body (Wasm validation
	// guarantees the value stack matches currentFunc.results).
	void emitImplicitFunctionReturn();

	// Emits the function prologue: the guest call-stack guard and the pending
	// check that turns a guard trap into an early return. Call before readCode().
	void emitFunctionEntry();

	// Emits the shared error epilogue (return zeroes) when any trap path was
	// emitted. Call after emitImplicitFunctionReturn().
	void emitTrapEpilogue();

	// OpcodeDispatcher interface
protected:
	// `internals.translatedTypes` is parallel to `module.typeIds` (filled in translateTypes).
	jit_type_t jitTypeForTypeIdx(WASM::TypeIdx idx) const;
	jit_type_t jitTypeForValueType(const WASM::ValueType& vt);
	static std::uint8_t continuationKindFor(const WASM::ValueType& vt);
	jit_value_t allocateContinuationBuffer(std::size_t count);
	void storeContinuationValues(const std::vector<jit_value_t>& values,
			const std::vector<WASM::StorageType>& types, jit_value_t buffer);
	void loadContinuationValues(jit_value_t buffer,
			const std::vector<WASM::StorageType>& types);
	void dropContinuationValues(jit_value_t buffer,
			const std::vector<WASM::StorageType>& types);

	void dispatchUnreachable() override;
	void dispatchNop() override;
	void dispatchBlock(const WASM::BlockType& arg) override;
	void dispatchLoop(const WASM::BlockType& arg) override;
	void dispatchIf(const WASM::BlockType& arg) override;
	void dispatchElse() override;
	void dispatchThrow(WASM::TagIdx arg) override;
	void dispatchThrowRef() override;
	void dispatchContNew(WASM::TypeIdx arg) override;
	void dispatchContBind(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override;
	void dispatchSuspend(WASM::TagIdx arg) override;
	void dispatchResume(WASM::TypeIdx arg, std::vector<WASM::ContHandlerClause>&& handlers) override;
	void dispatchResumeThrow(WASM::TypeIdx arg1, WASM::TagIdx arg2,
			std::vector<WASM::ContHandlerClause>&& handlers) override;
	void dispatchResumeThrowRef(WASM::TypeIdx arg,
			std::vector<WASM::ContHandlerClause>&& handlers) override;
	void emitContinuationResultHandling(const WASM::FuncType& func,
			const std::vector<WASM::ContHandlerClause>& handlers,
			jit_value_t resultAddr, jit_value_t resultBuffer,
			jit_value_t tagBuffer, size_t baseDepth);
	void dispatchEnd() override;
	void dispatchBr(WASM::LabelIdx arg) override;
	void dispatchBrIf(WASM::LabelIdx arg) override;
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) override;
	void dispatchReturn() override;
	void dispatchCall(WASM::FuncIdx arg) override;
	void dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override;
	void dispatchReturnCall(WASM::FuncIdx arg) override;
	void dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override;
	void dispatchCallRef(WASM::TypeIdx arg) override;
	void dispatchReturnCallRef(WASM::TypeIdx arg) override;
	void dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) override;
	void dispatchDrop() override;
	void dispatchSelect() override;
	void dispatchSelectT(std::vector<WASM::ValueType>&& arg) override;
	void dispatchLocalGet(WASM::LocalIdx arg) override;
	void dispatchLocalSet(WASM::LocalIdx arg) override;
	void dispatchLocalTee(WASM::LocalIdx arg) override;
	void dispatchGlobalGet(WASM::GlobalIdx arg) override;
	void dispatchGlobalSet(WASM::GlobalIdx arg) override;
	void dispatchTableGet(WASM::TableIdx arg) override;
	void dispatchTableSet(WASM::TableIdx arg) override;
	void dispatchI32Load(WASM::MemArg addr) override;
	void dispatchI64Load(WASM::MemArg addr) override;
	void dispatchF32Load(WASM::MemArg addr) override;
	void dispatchF64Load(WASM::MemArg addr) override;
	void dispatchI32Load8S(WASM::MemArg addr) override;
	void dispatchI32Load8U(WASM::MemArg addr) override;
	void dispatchI32Load16S(WASM::MemArg addr) override;
	void dispatchI32Load16U(WASM::MemArg addr) override;
	void dispatchI64Load8S(WASM::MemArg addr) override;
	void dispatchI64Load8U(WASM::MemArg addr) override;
	void dispatchI64Load16S(WASM::MemArg addr) override;
	void dispatchI64Load16U(WASM::MemArg addr) override;
	void dispatchI64Load32S(WASM::MemArg addr) override;
	void dispatchI64Load32U(WASM::MemArg addr) override;
	void dispatchI32Store(WASM::MemArg addr) override;
	void dispatchI64Store(WASM::MemArg addr) override;
	void dispatchF32Store(WASM::MemArg addr) override;
	void dispatchF64Store(WASM::MemArg addr) override;
	void dispatchI32Store8(WASM::MemArg addr) override;
	void dispatchI32Store16(WASM::MemArg addr) override;
	void dispatchI64Store8(WASM::MemArg addr) override;
	void dispatchI64Store16(WASM::MemArg addr) override;
	void dispatchI64Store32(WASM::MemArg addr) override;
	void dispatchMemorySize(WASM::MemIdx arg) override;
	void dispatchMemoryGrow(WASM::MemIdx arg) override;
	void dispatchI32Const(int32_t arg) override;
	void dispatchI64Const(int64_t arg) override;
	void dispatchF32Const(float arg) override;
	void dispatchF64Const(double arg) override;
	void dispatchI32Eqz() override;
	void dispatchI32Eq() override;
	void dispatchI32Ne() override;
	void dispatchI32LtS() override;
	void dispatchI32LtU() override;
	void dispatchI32GtS() override;
	void dispatchI32GtU() override;
	void dispatchI32LeS() override;
	void dispatchI32LeU() override;
	void dispatchI32GeS() override;
	void dispatchI32GeU() override;
	void dispatchI64Eqz() override;
	void dispatchI64Eq() override;
	void dispatchI64Ne() override;
	void dispatchI64LtS() override;
	void dispatchI64LtU() override;
	void dispatchI64GtS() override;
	void dispatchI64GtU() override;
	void dispatchI64LeS() override;
	void dispatchI64LeU() override;
	void dispatchI64GeS() override;
	void dispatchI64GeU() override;
	void dispatchF32Eq() override;
	void dispatchF32Ne() override;
	void dispatchF32Lt() override;
	void dispatchF32Gt() override;
	void dispatchF32Le() override;
	void dispatchF32Ge() override;
	void dispatchF64Eq() override;
	void dispatchF64Ne() override;
	void dispatchF64Lt() override;
	void dispatchF64Gt() override;
	void dispatchF64Le() override;
	void dispatchF64Ge() override;
	void dispatchI32Clz() override;
	void dispatchI32Ctz() override;
	void dispatchI32Popcnt() override;
	void dispatchI32Add() override;
	void dispatchI32Sub() override;
	void dispatchI32Mul() override;
	void dispatchI32DivS() override;
	void dispatchI32DivU() override;
	void dispatchI32RemS() override;
	void dispatchI32RemU() override;
	void dispatchI32And() override;
	void dispatchI32Or() override;
	void dispatchI32Xor() override;
	void dispatchI32Shl() override;
	void dispatchI32ShrS() override;
	void dispatchI32ShrU() override;
	void dispatchI32Rotl() override;
	void dispatchI32Rotr() override;
	void dispatchI64Clz() override;
	void dispatchI64Ctz() override;
	void dispatchI64Popcnt() override;
	void dispatchI64Add() override;
	void dispatchI64Sub() override;
	void dispatchI64Mul() override;
	void dispatchI64DivS() override;
	void dispatchI64DivU() override;
	void dispatchI64RemS() override;
	void dispatchI64RemU() override;
	void dispatchI64And() override;
	void dispatchI64Or() override;
	void dispatchI64Xor() override;
	void dispatchI64Shl() override;
	void dispatchI64ShrS() override;
	void dispatchI64ShrU() override;
	void dispatchI64Rotl() override;
	void dispatchI64Rotr() override;
	void dispatchF32Abs() override;
	void dispatchF32Neg() override;
	void dispatchF32Ceil() override;
	void dispatchF32Floor() override;
	void dispatchF32Trunc() override;
	void dispatchF32Nearest() override;
	void dispatchF32Sqrt() override;
	void dispatchF32Add() override;
	void dispatchF32Sub() override;
	void dispatchF32Mul() override;
	void dispatchF32Div() override;
	void dispatchF32Min() override;
	void dispatchF32Max() override;
	void dispatchF32Copysign() override;
	void dispatchF64Abs() override;
	void dispatchF64Neg() override;
	void dispatchF64Ceil() override;
	void dispatchF64Floor() override;
	void dispatchF64Trunc() override;
	void dispatchF64Nearest() override;
	void dispatchF64Sqrt() override;
	void dispatchF64Add() override;
	void dispatchF64Sub() override;
	void dispatchF64Mul() override;
	void dispatchF64Div() override;
	void dispatchF64Min() override;
	void dispatchF64Max() override;
	void dispatchF64Copysign() override;
	void dispatchI32WrapI64() override;
	void dispatchI32TruncF32S() override;
	void dispatchI32TruncF32U() override;
	void dispatchI32TruncF64S() override;
	void dispatchI32TruncF64U() override;
	void dispatchI64ExtendI32S() override;
	void dispatchI64ExtendI32U() override;
	void dispatchI64TruncF32S() override;
	void dispatchI64TruncF32U() override;
	void dispatchI64TruncF64S() override;
	void dispatchI64TruncF64U() override;
	void dispatchF32ConvertI32S() override;
	void dispatchF32ConvertI32U() override;
	void dispatchF32ConvertI64S() override;
	void dispatchF32ConvertI64U() override;
	void dispatchF32DemoteF64() override;
	void dispatchF64ConvertI32S() override;
	void dispatchF64ConvertI32U() override;
	void dispatchF64ConvertI64S() override;
	void dispatchF64ConvertI64U() override;
	void dispatchF64PromoteF32() override;
	void dispatchI32ReinterpretF32() override;
	void dispatchI64ReinterpretF64() override;
	void dispatchF32ReinterpretI32() override;
	void dispatchF64ReinterpretI64() override;
	void dispatchI32Extend8S() override;
	void dispatchI32Extend16S() override;
	void dispatchI64Extend8S() override;
	void dispatchI64Extend16S() override;
	void dispatchI64Extend32S() override;
	void dispatchRefNull(const WASM::HeapType& arg) override;
	void dispatchRefIsNull() override;
	void dispatchRefFunc(WASM::FuncIdx arg) override;
	void dispatchRefEq() override;
	void dispatchRefAsNonNull() override;
	void dispatchBrOnNull(WASM::LabelIdx arg) override;
	void dispatchBrOnNonNull(WASM::LabelIdx arg) override;
	void dispatchStructNew(WASM::TypeIdx arg) override;
	void dispatchStructNewDefault(WASM::TypeIdx arg) override;
	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchArrayNew(WASM::TypeIdx arg) override;
	void dispatchArrayNewDefault(WASM::TypeIdx arg) override;
	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override;
	// More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2).
	void dispatchArrayNewArray(WASM::TypeIdx dst, WASM::TypeIdx src) override;
	void dispatchArrayNewMemory(WASM::TypeIdx dst, uint32_t src) override;
	void dispatchArrayNewTable(WASM::TypeIdx dst, uint32_t src) override;
	void dispatchArrayGet(WASM::TypeIdx arg) override;
	void dispatchArrayGetS(WASM::TypeIdx arg) override;
	void dispatchArrayGetU(WASM::TypeIdx arg) override;
	void dispatchArraySet(WASM::TypeIdx arg) override;
	void dispatchArrayLen() override;
	void dispatchArrayFill(WASM::TypeIdx arg) override;
	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override;
	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchRefTest(const WASM::HeapType& arg) override;
	void dispatchRefTestNull(const WASM::HeapType& arg) override;
	void dispatchRefCast(const WASM::HeapType& arg) override;
	void dispatchRefCastNull(const WASM::HeapType& arg) override;
	void dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	void dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	void dispatchAnyConvertExtern() override;
	void dispatchExternConvertAny() override;
	void dispatchRefI31() override;
	void dispatchI31GetS() override;
	void dispatchI31GetU() override;

	// Stringref core instructions (docs/proposals/STRINGREF.md STR-3).
	void dispatchStringNewUtf8(WASM::MemIdx arg) override;
	void dispatchStringNewWtf16(WASM::MemIdx arg) override;
	void dispatchStringNewLossyUtf8(WASM::MemIdx arg) override;
	void dispatchStringNewWtf8(WASM::MemIdx arg) override;
	void dispatchStringConst(uint32_t idx) override;
	void dispatchStringMeasureUtf8() override;
	void dispatchStringMeasureWtf8() override;
	void dispatchStringMeasureWtf16() override;
	void dispatchStringEncodeUtf8(WASM::MemIdx arg) override;
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx arg) override;
	void dispatchStringEncodeWtf8(WASM::MemIdx arg) override;
	void dispatchStringEncodeWtf16(WASM::MemIdx arg) override;
	void dispatchStringConcat() override;
	void dispatchStringEq() override;
	void dispatchStringIsUsvSequence() override;

	// stringview facility (docs/proposals/STRINGREF.md STR-4)
	void dispatchStringAsWtf8() override;
	void dispatchStringViewWtf8Advance() override;
	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx arg) override;
	void dispatchStringViewWtf8Slice() override;
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx arg) override;
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx arg) override;
	void dispatchStringAsWtf16() override;
	void dispatchStringViewWtf16Length() override;
	void dispatchStringViewWtf16GetCodeUnit() override;
	void dispatchStringViewWtf16Encode(WASM::MemIdx arg) override;
	void dispatchStringViewWtf16Slice() override;
	void dispatchStringAsIter() override;
	void dispatchStringViewIterNext() override;
	void dispatchStringViewIterAdvance() override;
	void dispatchStringViewIterRewind() override;
	void dispatchStringViewIterSlice() override;

	// GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
	void dispatchStringNewUtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringNewWtf16Array(WASM::TypeIdx arg) override;
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx arg) override;
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringNewWtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringNewArrayImpl(WASM::TypeIdx arg, void* helper, int mode);
	void dispatchStringEncodeArrayImpl(WASM::TypeIdx arg, void* helper, int mode);
	void dispatchI32TruncSatF32S() override;
	void dispatchI32TruncSatF32U() override;
	void dispatchI32TruncSatF64S() override;
	void dispatchI32TruncSatF64U() override;
	void dispatchI64TruncSatF32S() override;
	void dispatchI64TruncSatF32U() override;
	void dispatchI64TruncSatF64S() override;
	void dispatchI64TruncSatF64U() override;
	void dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) override;
	void dispatchDataDrop(uint32_t arg) override;
	void dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) override;
	void dispatchMemoryFill(WASM::MemIdx arg) override;
	void dispatchMemoryDiscard(WASM::MemIdx arg) override;
	void dispatchRoundingVariant(uint32_t sub) override;
	void dispatchI64Add128() override;
	void dispatchI64Sub128() override;
	void dispatchI64MulWideS() override;
	void dispatchI64MulWideU() override;
	void dispatchI32Itt(uint32_t immediate) override;
	void dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) override;
	void dispatchElemDrop(uint32_t arg) override;
	void dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) override;
	void dispatchTableGrow(WASM::TableIdx arg) override;
	void dispatchTableSize(WASM::TableIdx arg) override;
	void dispatchTableFill(WASM::TableIdx arg) override;
	void dispatchV128Load(WASM::MemArg arg) override;
	void dispatchV128Load8x8S(WASM::MemArg arg) override;
	void dispatchV128Load8x8U(WASM::MemArg arg) override;
	void dispatchV128Load16x4S(WASM::MemArg arg) override;
	void dispatchV128Load16x4U(WASM::MemArg arg) override;
	void dispatchV128Load32x2S(WASM::MemArg arg) override;
	void dispatchV128Load32x2U(WASM::MemArg arg) override;
	void dispatchV128Load8Splat(WASM::MemArg arg) override;
	void dispatchV128Load16Splat(WASM::MemArg arg) override;
	void dispatchV128Load32Splat(WASM::MemArg arg) override;
	void dispatchV128Load64Splat(WASM::MemArg arg) override;
	void dispatchV128Load32Zero(WASM::MemArg arg) override;
	void dispatchV128Load64Zero(WASM::MemArg arg) override;
	void dispatchV128Store(WASM::MemArg arg) override;
	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override;
	void dispatchV128Const(std::span<uint8_t> arg) override;
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override;
	void dispatchI8x16Swizzle() override;
	void dispatchI8x16Splat() override;
	void dispatchI16x8Splat() override;
	void dispatchI32x4Splat() override;
	void dispatchI64x2Splat() override;
	void dispatchF32x4Splat() override;
	void dispatchF64x2Splat() override;
	void dispatchI8x16ExtractLaneS(uint8_t arg) override;
	void dispatchI8x16ExtractLaneU(uint8_t arg) override;
	void dispatchI8x16ReplaceLane(uint8_t arg) override;
	void dispatchI16x8ExtractLaneS(uint8_t arg) override;
	void dispatchI16x8ExtractLaneU(uint8_t arg) override;
	void dispatchI16x8ReplaceLane(uint8_t arg) override;
	void dispatchI32x4ExtractLane(uint8_t arg) override;
	void dispatchI32x4ReplaceLane(uint8_t arg) override;
	void dispatchI64x2ExtractLane(uint8_t arg) override;
	void dispatchI64x2ReplaceLane(uint8_t arg) override;
	void dispatchF32x4ExtractLane(uint8_t arg) override;
	void dispatchF32x4ReplaceLane(uint8_t arg) override;
	void dispatchF64x2ExtractLane(uint8_t arg) override;
	void dispatchF64x2ReplaceLane(uint8_t arg) override;
	void dispatchI8x16Eq() override;
	void dispatchI8x16Ne() override;
	void dispatchI8x16LtS() override;
	void dispatchI8x16LtU() override;
	void dispatchI8x16GtS() override;
	void dispatchI8x16GtU() override;
	void dispatchI8x16LeS() override;
	void dispatchI8x16LeU() override;
	void dispatchI8x16GeS() override;
	void dispatchI8x16GeU() override;
	void dispatchI16x8Eq() override;
	void dispatchI16x8Ne() override;
	void dispatchI16x8LtS() override;
	void dispatchI16x8LtU() override;
	void dispatchI16x8GtS() override;
	void dispatchI16x8GtU() override;
	void dispatchI16x8LeS() override;
	void dispatchI16x8LeU() override;
	void dispatchI16x8GeS() override;
	void dispatchI16x8GeU() override;
	void dispatchI32x4Eq() override;
	void dispatchI32x4Ne() override;
	void dispatchI32x4LtS() override;
	void dispatchI32x4LtU() override;
	void dispatchI32x4GtS() override;
	void dispatchI32x4GtU() override;
	void dispatchI32x4LeS() override;
	void dispatchI32x4LeU() override;
	void dispatchI32x4GeS() override;
	void dispatchI32x4GeU() override;
	void dispatchF32x4Eq() override;
	void dispatchF32x4Ne() override;
	void dispatchF32x4Lt() override;
	void dispatchF32x4Gt() override;
	void dispatchF32x4Le() override;
	void dispatchF32x4Ge() override;
	void dispatchF64x2Eq() override;
	void dispatchF64x2Ne() override;
	void dispatchF64x2Lt() override;
	void dispatchF64x2Gt() override;
	void dispatchF64x2Le() override;
	void dispatchF64x2Ge() override;
	void dispatchV128Not() override;
	void dispatchV128And() override;
	void dispatchV128AndNot() override;
	void dispatchV128Or() override;
	void dispatchV128Xor() override;
	void dispatchV128Bitselect() override;
	void dispatchV128AnyTrue() override;
	void dispatchI8x16Abs() override;
	void dispatchI8x16Neg() override;
	void dispatchI8x16Popcnt() override;
	void dispatchI8x16AllTrue() override;
	void dispatchI8x16Bitmask() override;
	void dispatchI8x16NarrowI16x8S() override;
	void dispatchI8x16NarrowI16x8U() override;
	void dispatchI8x16Shl() override;
	void dispatchI8x16ShrS() override;
	void dispatchI8x16ShrU() override;
	void dispatchI8x16Add() override;
	void dispatchI8x16AddSatS() override;
	void dispatchI8x16AddSatU() override;
	void dispatchI8x16Sub() override;
	void dispatchI8x16SubSatS() override;
	void dispatchI8x16SubSatU() override;
	void dispatchI8x16MinS() override;
	void dispatchI8x16MinU() override;
	void dispatchI8x16MaxS() override;
	void dispatchI8x16MaxU() override;
	void dispatchI8x16AvgrU() override;
	void dispatchI16x8ExtAddPairwiseI8x16S() override;
	void dispatchI16x8ExtAddPairwiseI8x16U() override;
	void dispatchI16x8Abs() override;
	void dispatchI16x8Neg() override;
	void dispatchI16x8Q15MulRSatS() override;
	void dispatchI16x8AllTrue() override;
	void dispatchI16x8Bitmask() override;
	void dispatchI16x8NarrowI32x4S() override;
	void dispatchI16x8NarrowI32x4U() override;
	void dispatchI16x8ExtendLowI8x16S() override;
	void dispatchI16x8ExtendHighI8x16S() override;
	void dispatchI16x8ExtendLowI8x16U() override;
	void dispatchI16x8ExtendHighI8x16U() override;
	void dispatchI16x8Shl() override;
	void dispatchI16x8ShrS() override;
	void dispatchI16x8ShrU() override;
	void dispatchI16x8Add() override;
	void dispatchI16x8AddSatS() override;
	void dispatchI16x8AddSatU() override;
	void dispatchI16x8Sub() override;
	void dispatchI16x8SubSatS() override;
	void dispatchI16x8SubSatU() override;
	void dispatchI16x8Mul() override;
	void dispatchI16x8MinS() override;
	void dispatchI16x8MinU() override;
	void dispatchI16x8MaxS() override;
	void dispatchI16x8MaxU() override;
	void dispatchI16x8AvgrU() override;
	void dispatchI16x8ExtMulLowI8x16S() override;
	void dispatchI16x8ExtMulHighI8x16S() override;
	void dispatchI16x8ExtMulLowI8x16U() override;
	void dispatchI16x8ExtMulHighI8x16U() override;
	void dispatchI32x4ExtAddPairwiseI16x8S() override;
	void dispatchI32x4ExtAddPairwiseI16x8U() override;
	void dispatchI32x4Abs() override;
	void dispatchI32x4Neg() override;
	void dispatchI32x4AllTrue() override;
	void dispatchI32x4Bitmask() override;
	void dispatchI32x4ExtendLowI16x8S() override;
	void dispatchI32x4ExtendHighI16x8S() override;
	void dispatchI32x4ExtendLowI16x8U() override;
	void dispatchI32x4ExtendHighI16x8U() override;
	void dispatchI32x4Shl() override;
	void dispatchI32x4ShrS() override;
	void dispatchI32x4ShrU() override;
	void dispatchI32x4Add() override;
	void dispatchI32x4Sub() override;
	void dispatchI32x4Mul() override;
	void dispatchI32x4MinS() override;
	void dispatchI32x4MinU() override;
	void dispatchI32x4MaxS() override;
	void dispatchI32x4MaxU() override;
	void dispatchI32x4DotI16x8S() override;
	void dispatchI32x4ExtMulLowI16x8S() override;
	void dispatchI32x4ExtMulHighI16x8S() override;
	void dispatchI32x4ExtMulLowI16x8U() override;
	void dispatchI32x4ExtMulHighI16x8U() override;
	void dispatchI64x2Abs() override;
	void dispatchI64x2Neg() override;
	void dispatchI64x2AllTrue() override;
	void dispatchI64x2Bitmask() override;
	void dispatchI64x2ExtendLowI32x4S() override;
	void dispatchI64x2ExtendHighI32x4S() override;
	void dispatchI64x2ExtendLowI32x4U() override;
	void dispatchI64x2ExtendHighI32x4U() override;
	void dispatchI64x2Shl() override;
	void dispatchI64x2ShrS() override;
	void dispatchI64x2ShrU() override;
	void dispatchI64x2Add() override;
	void dispatchI64x2Sub() override;
	void dispatchI64x2Mul() override;
	void dispatchI64x2Eq() override;
	void dispatchI64x2Ne() override;
	void dispatchI64x2LtS() override;
	void dispatchI64x2GtS() override;
	void dispatchI64x2LeS() override;
	void dispatchI64x2GeS() override;
	void dispatchI64x2ExtMulLowI32x4S() override;
	void dispatchI64x2ExtMulHighI32x4S() override;
	void dispatchI64x2ExtMulLowI32x4U() override;
	void dispatchI64x2ExtMulHighI32x4U() override;
	void dispatchF32x4Ceil() override;
	void dispatchF32x4Floor() override;
	void dispatchF32x4Trunc() override;
	void dispatchF32x4Nearest() override;
	void dispatchF32x4Abs() override;
	void dispatchF32x4Neg() override;
	void dispatchF32x4Sqrt() override;
	void dispatchF32x4Add() override;
	void dispatchF32x4Sub() override;
	void dispatchF32x4Mul() override;
	void dispatchF32x4Div() override;
	void dispatchF32x4Min() override;
	void dispatchF32x4Max() override;
	void dispatchF32x4PMin() override;
	void dispatchF32x4PMax() override;
	void dispatchF64x2Ceil() override;
	void dispatchF64x2Floor() override;
	void dispatchF64x2Trunc() override;
	void dispatchF64x2Nearest() override;
	void dispatchF64x2Abs() override;
	void dispatchF64x2Neg() override;
	void dispatchF64x2Sqrt() override;
	void dispatchF64x2Add() override;
	void dispatchF64x2Sub() override;
	void dispatchF64x2Mul() override;
	void dispatchF64x2Div() override;
	void dispatchF64x2Min() override;
	void dispatchF64x2Max() override;
	void dispatchF64x2PMin() override;
	void dispatchF64x2PMax() override;
	void dispatchI32x4TruncSatF32x4S() override;
	void dispatchI32x4TruncSatF32x4U() override;
	void dispatchF32x4ConvertI32x4S() override;
	void dispatchF32x4ConvertI32x4U() override;
	void dispatchI32x4TruncSatF64x2SZero() override;
	void dispatchI32x4TruncSatF64x2UZero() override;
	void dispatchF64x2ConvertLowI32x4S() override;
	void dispatchF64x2ConvertLowI32x4U() override;
	void dispatchF32x4DemoteF64x2Zero() override;
	void dispatchF64x2PromoteLowF32x4() override;

	// Relaxed SIMD (0x100-0x113): SD5 picks the strict result for each.
	void dispatchI8x16RelaxedSwizzle() override;
	void dispatchI32x4RelaxedTruncF32x4S() override;
	void dispatchI32x4RelaxedTruncF32x4U() override;
	void dispatchI32x4RelaxedTruncF64x2SZero() override;
	void dispatchI32x4RelaxedTruncF64x2UZero() override;
	void dispatchF32x4RelaxedMadd() override;
	void dispatchF32x4RelaxedNmadd() override;
	void dispatchF64x2RelaxedMadd() override;
	void dispatchF64x2RelaxedNmadd() override;
	void dispatchI8x16RelaxedLaneselect() override;
	void dispatchI16x8RelaxedLaneselect() override;
	void dispatchI32x4RelaxedLaneselect() override;
	void dispatchI64x2RelaxedLaneselect() override;
	void dispatchF32x4RelaxedMin() override;
	void dispatchF32x4RelaxedMax() override;
	void dispatchF64x2RelaxedMin() override;
	void dispatchF64x2RelaxedMax() override;
	void dispatchI16x8RelaxedQ15MulRS() override;
	void dispatchI16x8RelaxedDotI8x16I7x16S() override;
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override;
	void dispatchMemoryAtomicNotify(WASM::MemArg arg) override;
	void dispatchMemoryAtomicWait32(WASM::MemArg arg) override;
	void dispatchMemoryAtomicWait64(WASM::MemArg arg) override;
	void dispatchAtomicFence(uint8_t ordering) override;
	void dispatchAtomicPause() override;
	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override;
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override;
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override;
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override;
	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override;
	void dispatchI32AtomicLoad(WASM::MemArg arg) override;
	void dispatchI64AtomicLoad(WASM::MemArg arg) override;
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override;
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override;
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override;
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override;
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override;
	void dispatchI32AtomicStore(WASM::MemArg arg) override;
	void dispatchI64AtomicStore(WASM::MemArg arg) override;
	void dispatchI32AtomicStore8(WASM::MemArg arg) override;
	void dispatchI32AtomicStore16(WASM::MemArg arg) override;
	void dispatchI64AtomicStore8(WASM::MemArg arg) override;
	void dispatchI64AtomicStore16(WASM::MemArg arg) override;
	void dispatchI64AtomicStore32(WASM::MemArg arg) override;
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override;
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override;
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override;
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override;
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override;
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override;
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override;
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override;
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override;
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override;
};

}

#endif // LIBJITOPCODEDISPATCHER_HPP
