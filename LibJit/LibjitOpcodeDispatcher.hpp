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

/*!
  \file LibjitOpcodeDispatcher.hpp
  \brief Lowering every decoded instruction into this backend's intermediate form.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {

//! One frame of the structured-control stack (`block` / `loop` / `if` / `try_table`).
struct ControlBlock {
	//! Which structured-control construct opened this frame.
	enum Kind {
		Block,    //!< Plain `block` … `end`.
		Loop,     //!< `loop` … `end` (a branch targets the loop head).
		If,       //!< `if` … [`else` …] `end`.
		TryTable  //!< `try_table` … `end` with static catch clauses.
	};
	Kind kind; //!< The construct that opened this frame.

	//! For Block/If: a forward label at the end (bound when we see 'end').
	//! For Loop: a backward label at the start (bound when we see 'loop').
	//! A 'br N' instruction branches to controlStack[top - N].label.
	jit_label_t label;

	//! For 'if': we also need a label for the 'else' branch, so that
	//! the condition check can jump forward to it if false.
	jit_label_t elseLabel;

	//! The value stack depth when this block was entered. Used to
	//! validate and restore the stack at 'else' and 'end' boundaries.
	size_t stackDepth;

	//! The types this block is expected to produce on exit.
	//! A 'br' to this block must leave these types on the stack.
	std::vector<WASM::StorageType> resultTypes;
	//! Types this block consumes on entry (block parameters).
	std::vector<WASM::StorageType> paramTypes;
	//! Frame slots that hold the block's result values across a branch.
	std::vector<jit_value_t> resultSlots;
	//! Frame slots that hold the block's parameter values across a branch.
	std::vector<jit_value_t> paramSlots;
	//! True once an `else` has been seen for an `If` frame.
	bool hasElse = false;
	//! try_table only: the decoded catch clauses. They are not a runtime
	//! construct — throw sites dispatch to them statically (EXCEPTIONS.md
	//! EXD2). `labelIdx` is relative to this frame's *parent*.
	std::vector<WASM::CatchClause> catchClauses;
};

//! Drives WASM::OpcodeDispatcher::readCode for LibJIT: decodes the bytecode
//! stream and emits jit_insn_* into the current jit_function_t. Holds the
//! transient compilation state (locals, stacks, module context) that
//! ModuleCompiler used to pass through a monolithic dispatchOpcode.
class OpcodeDispatcher : public WASM::OpcodeDispatcher
{
private:
	jit_context_t context; //!< LibJIT compilation context for this module.
	jit_function_t function; //!< LibJIT function currently being built.
	LibJitTypeTranslator& typeTranslator; //!< Wasm → LibJIT type mapping for this compile.
	WASM::ModuleInstance& instance; //!< Instance whose entities the body may touch.
	WASM::ModuleInstanceInternals& internals; //!< Backend-private tables for `instance`.
	//! Back-reference to the parsed module (types, imports, bodies). Not owned.
	const WASM::Module& module;
	const WASM::FuncType& currentFunc; //!< Signature of the body being compiled.
	uint32_t importedFuncCount; //!< Number of imported functions preceding local bodies.
	//! Global function index of the body being compiled (imports first), and its
	//! diagnostic label (`function 3 ($name)`). Emitted traps carry the label so
	//! a runtime failure names the function, not just the trap kind
	//! (docs/proposals/EXTENDED_NAME_SECTION.md ENS-5).
	uint32_t functionIndex;
	std::string functionLabel; //!< Diagnostic label for `functionIndex` (`function N ($name)`).
	//! The name-section spelling of `functionIndex`, interned in the instance for
	//! the generated code's lifetime; null when the module has no name for it.
	//! Carried as the DG-5 trap site (with `functionIndex`/`instructionOffset`).
	const char* functionName = nullptr;
	std::vector<jit_value_t>& locals; //!< LibJIT locals for parameters and declared locals.
	std::vector<jit_value_t>& valueStack; //!< Model of the Wasm operand stack during lowering.
	std::vector<ControlBlock>& controlStack; //!< Nested structured-control frames.

	//! Push `v` onto the model operand stack.
	void pushValue(jit_value_t v);
	//! Push a reference whose ownership the stack takes over (no clone).
	void pushOwnedValue(jit_value_t v);
	//! Release the operand stack's ownership of a popped reference.
	void emitDropValue(jit_value_t v);
	//! Release every reference-typed non-parameter local (function exit).
/**
 * \brief Emits LibJIT IR for DropRefLocals.
 */
	void emitDropRefLocals();
	//! Pop and return the top of the model operand stack (placeholder when unreachable).
	jit_value_t popValue();
	//! Emits the function's epilogue (a `return`). Does nothing in unreachable
	//! code or when the model stack is short (dead tail). With `consume` false
	//! the return operands are left on the model stack (for a conditional
	//! branch to the function's implicit block, whose fallthrough needs them).
	void emitFunctionReturn(bool consume = true);
	//! Emits a native tail call (LibJIT `JIT_CALL_TAIL`) when the target is this
	//! very function — the only case this LibJIT build lowers reliably (see
	//! docs/TAILCALLS.md §6). Returns false when the caller must fall back to
	//! `call` + `return`.
	bool emitDirectTailCall(WASM::FuncIdx funcIdx);
	//! True when `calleeSig` has exactly this function's parameter and result
	//! types. The trampoline no longer needs it (TC-3 dispatches through the
	//! callee type's thunk), but it is part of the shipped LibJit symbol set, so
	//! it stays for the additive-only ABI rule (docs/API_ABI_STABILITY.md G2).
	bool signatureIsUniform(const WASM::FuncType& calleeSig) const;
	//! Publishes {target, args, pending} to the current execution state's
	//! trampoline state and returns a dummy value; the driver re-dispatches
	//! through the target type's thunk (docs/TAILCALLS.md TC-2/TC-3). Callers
	//! must have verified WASM::tailCallSlotsSupported() and popped `args`.
	void emitTailCallViaPending(jit_value_t targetCallablePtr, const std::vector<jit_value_t>& args);
	//! Semantic fallback for a tail call the trampoline cannot take: a
	//! `call` + `return` with the callee's results becoming this function's.
	//! Used for a native import on the direct path and for a callable with no
	//! dispatch thunk on the indirect/ref paths.
	void emitTailCallFallback(WASM::TypeIdx typeIdx, jit_value_t callablePtr,
							  const std::vector<jit_value_t>& args);
	//! Emits `return` of a zero value of this function's result type.
/**
 * \brief Emits LibJIT IR for DummyReturn.
 */
	void emitDummyReturn();
	//! Emits the jump for a branch target; a target past the control stack is
	//! the function's implicit block (i.e. a return).
	void emitBranchToLabel(WASM::LabelIdx arg);
	//! Marks the rest of the current block unreachable (after an unconditional
	//! transfer). In unreachable code the Wasm operand stack is polymorphic, so
	//! popValue() yields placeholders instead of underflowing.
	void markUnreachable();
	//! True after an unconditional transfer until the enclosing block ends.
	bool unreachableCode = false;

	//! Branch hints for the function being compiled, ordered by byte offset
	//! (already rebased onto the opcode stream). Empty when the module carries
	//! no `metadata.code.branch_hint` section, in which case codegen is
	//! byte-identical to the hint-less path.
	std::vector<WASM::BranchHint> branchHints;
	//! Returns the hint attached to the instruction currently being dispatched,
	//! or nullptr if there is none. Purely an optimisation input; never changes
	//! results.
	const WASM::BranchHint* currentBranchHint() const;
	//! A zero (or null) constant of LibJIT type `t`, used as a placeholder or trap return.
	jit_value_t zeroConstantForType(jit_type_t t);

	//! \par SIMD lowering (docs/SIMD_LIBJIT.md §6)
	//!
	//! A `v128` operand is a struct-typed LibJIT value whose 16 bytes live in a
	//! frame slot; helpers never see the value, only its *address* (§6.3), and
	//! they write their result into a fresh slot. The mechanics were verified
	//! against this LibJIT build before the design was written down:
	//!   - `jit_insn_address_of` works on a struct parameter, on a struct local,
	//!     and on `jit_insn_load` of either;
	//!   - `jit_insn_store(local, value)` works for a struct local (so `local.set`
	//!     of a `v128` is fine);
	//!   - `jit_insn_store_relative` does **not** write a `jit_value_create`d
	//!     aggregate, so `v128.const` must go through a helper, not inline stores.
	jit_type_t v128Type();
	//! A fresh 16-byte slot for a `v128` result.
	jit_value_t newV128Slot();
	//! Address of a global's 16 bytes in the VMContext's `globals` array. The
	//! `v128` member sits at offset 0 of the `Value` union, so this is also the
	//! pointer the load/store helpers accept (SIMD-L5).
	jit_value_t globalSlotAddress(WASM::GlobalIdx arg);
	//! Calls a helper through a memoised signature (§6.4) and returns its result
	//! (`void` for the out-slot shapes — the caller then pushes the slot).
	jit_value_t callSimdHelper(void* fptr, jit_type_t ret,
			std::span<const jit_type_t> params, std::span<const jit_value_t> args);
	//! The common shape: leading operands, then the result slot's address.
	jit_value_t callSimdOutHelper(void* fptr, jit_value_t out,
			std::span<const jit_type_t> leadingTypes, std::span<const jit_value_t> leadingArgs);
	//! The `v128.load*` / `v128.store*` shapes. The effective address is computed
	//! and bounds-checked by `effectiveMemoryAddress` — the same path the scalar
	//! loads take, trap included — so the helpers themselves never trap and need
	//! no context (docs/SIMD_LIBJIT.md §6.3). Operand order is wasm's: for a lane
	//! or store form the vector is on top, the address below it.
	void emitSimdLoad(const WASM::MemArg& ma, jit_nint width, void* helper);
	//! `v128.load*_lane`: load `width` bytes into lane `lane` of the vector on top.
	void emitSimdLoadLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper);
	//! `v128.store*`: store the top vector's low `width` bytes to memory.
	void emitSimdStore(const WASM::MemArg& ma, jit_nint width, void* helper);
	//! `v128.store*_lane`: store one lane of the top vector to memory.
	void emitSimdStoreLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper);
	//! The per-lane shapes: two vectors in and one out (used for the arithmetic
	//! *and* for the mask-producing comparisons — the difference is entirely in
	//! the helper), one vector in and one out, and the three-operand bitselect.
	void emitSimdBinaryHelper(void* helper);
	//! Unary SIMD helper: one vector in, one vector out.
	void emitSimdUnaryHelper(void* helper);
	//! Ternary SIMD helper (e.g. `v128.bitselect`): three vectors in, one out.
	void emitSimdTernaryHelper(void* helper);
	//! A shift takes the vector plus a scalar count; a reduction yields a wasm i32
	//! rather than a vector, so it has no out slot at all.
	void emitSimdShiftHelper(void* helper);
	//! SIMD reduction to i32 (no out-slot): pop a vector, push the helper's i32.
	void emitSimdReduceI32Helper(void* helper);
	//! Signature cache: the SIMD ops share a handful of shapes, so memoise them
	//! instead of creating (and leaking) one signature per instruction (§6.4).
	std::vector<std::pair<std::vector<jit_type_t>, jit_type_t>> simdSignatureCache;
	//! Memoised LibJIT struct type for `v128` (null until first use).
	jit_type_t simdCachedType = nullptr;
	//! Reinterpret / cast a reference-typed value to `targetType` without changing bits.
	jit_value_t castRefValue(jit_value_t value, jit_type_t targetType);
	//! Narrow a reference-typed value to `void*` for helper and store calls.
	jit_value_t refAsVoidPtr(jit_value_t value);
	//! Emits the reference-counting write barrier (`wasm_ref_store`): retain
	//! `newRef`, store it at `slotAddr`, release the value it overwrote. Used
	//! for every reference-typed slot store (docs/proposals/GC.md D15/RC-1).
	void emitRefStore(jit_value_t slotAddr, jit_value_t newRef);
	//! `wasm_ref_clone` / `wasm_ref_drop` (retain / release), for slot stores
	//! that address a LibJIT local rather than memory (RC-5).
	jit_value_t emitRefClone(jit_value_t ref);
	//! Call `wasm_ref_drop` on `ref` (release one ownership count).
	void emitRefDrop(jit_value_t ref);
	//! A typed null reference constant of LibJIT reference type `refType`.
	jit_value_t typedNullRef(jit_type_t refType);
	//! Emit `ref.test` / nullability check of `ref` against `heapType`.
	jit_value_t emitRefTypeTest(jit_value_t ref, const WASM::HeapType& heapType, bool nullable);
	//! LibJIT element type of table `arg`.
	jit_type_t tableElementJitType(WASM::TableIdx arg) const;
	//! LibJIT reference type for heap type `ht` (nullable when `nullable`).
	jit_type_t jitRefTypeForHeapType(const WASM::HeapType& ht, bool nullable);
	//! LibJIT `(ref [null] $struct)` type for type index `arg`.
	jit_type_t structRefType(WASM::TypeIdx arg, bool nullable) const;
	//! LibJIT `(ref [null] $array)` type for type index `arg`.
	jit_type_t arrayRefType(WASM::TypeIdx arg, bool nullable) const;
	//! Canonical struct type view for module type index `arg`.
	WASM::CoreTypeView structTypeForIndex(WASM::TypeIdx arg) const;
	//! Canonical array type view for module type index `arg`.
	WASM::CoreTypeView arrayTypeForIndex(WASM::TypeIdx arg) const;
	//! LibJIT type of the element of array type `arg`.
	jit_type_t arrayElementJitType(WASM::TypeIdx arg) const;
	//! Byte offset of field `fieldIndex` inside struct type `arg`.
	jit_nint structFieldOffset(WASM::TypeIdx arg, uint32_t fieldIndex) const;
	//! Byte offset of the length word inside an array of type `arg`.
	jit_nint arrayLengthOffset(WASM::TypeIdx arg) const;
	//! Byte offset of the inline element payload inside an array of type `arg`.
	jit_nint arrayDataOffset(WASM::TypeIdx arg) const;
	//! `ref + dataOffset`, typed as a pointer to the array's element type.
	//! Arrays store their elements inline (docs/proposals/GC.md D13/GC-9), so
	//! there is no `data` field to load.
	jit_value_t arrayElementBase(jit_value_t arrayRef, WASM::TypeIdx arg);
	//! Layout of a multi-result function's result tuple. A multi-result
	//! signature carries a trailing `void*` result buffer instead of returning
	//! an aggregate by value (see LibJitTypeTranslation.cpp); this struct type
	//! describes that buffer.
	jit_type_t resultTupleType(const WASM::FuncType& wasm_func);
	//! Writes the top `resultCount` values into `out`, the caller-provided
	//! result buffer (a multi-result function's trailing parameter).
	void packReturnValues(jit_type_t tupleType, jit_value_t out, size_t resultCount);
	//! Pushes a call's results. For a multi-result callee `outBuffer` is the
	//! address of the caller's result tuple and `ret` is unused (the callee
	//! returns void); for 0/1 results `outBuffer` is null.
	void pushCallResults(const WASM::FuncType& calleeSig, jit_value_t ret, jit_value_t outBuffer);
	//! Allocates the caller-side result tuple in this function's frame and
	//! returns its address, or null when `calleeSig` has 0/1 results.
	jit_value_t newResultBuffer(const WASM::FuncType& calleeSig);
	//! Parameter storage types implied by block type `bt`.
	std::vector<WASM::StorageType> parameterTypesForBlockType(const WASM::BlockType& bt) const;
	//! Allocate one frame local per type in `types` (branch / block parameter slots).
	std::vector<jit_value_t> createSlotsForTypes(const std::vector<WASM::StorageType>& types);
	//! Pop `|slots|` values from the model stack and store them into `slots`.
	void storeStackTopToSlots(const std::vector<jit_value_t>& slots);
	//! Push the values currently held in `slots` onto the model stack.
	void restoreValuesFromSlots(const std::vector<jit_value_t>& slots);
	//! Truncate or grow the model operand stack to `newSize` (unreachable placeholders).
	void resizeValueStack(size_t newSize);
	//! Control frame targeted by relative label `arg` (must be in range).
	ControlBlock& branchTarget(WASM::LabelIdx arg);
	//! Result (or loop parameter) types a branch to `target` must leave on the stack.
	const std::vector<WASM::StorageType>& branchTypesForTarget(const ControlBlock& target) const;
	//! Frame slots a branch to `target` writes before jumping.
	const std::vector<jit_value_t>& branchSlotsForTarget(const ControlBlock& target) const;
	//! Store branch values into `target`'s slots and jump to its label.
	void emitBranchToTarget(ControlBlock& target);
	//! Module-local type index of function `funcIdx` (imports first).
	WASM::TypeIdx functionTypeIndexForFunc(WASM::FuncIdx funcIdx) const;
	//! Function signature for module type index `typeIdx`.
	WASM::FuncType functionSignatureForType(WASM::TypeIdx typeIdx) const;
	//! `cont $ft`: resolve the module-local function type index stored in the
	//! canonical continuation type (STACK_SWITCHING.md SS-1).
	WASM::TypeIdx continuationFuncIndex(WASM::TypeIdx contTypeIdx) const;
	//! Function signature wrapped by continuation type `contTypeIdx`.
	WASM::FuncType continuationSignature(WASM::TypeIdx contTypeIdx) const;
	//! Pointer to the callable for function index `funcIdx` (import or local).
	jit_value_t callablePointerForFuncIndex(WASM::FuncIdx funcIdx);
	void dispatchCallThroughCallable(jit_value_t callablePtr, WASM::TypeIdx typeIdx); //!< Lower this opcode into the backend IR.
	//! Emit a trap of `kind` (docs/SAFETY.md §2.4).
	void emitTrap(WASM::TrapKind kind);
	//! Emit a trap of `kind`; `detail` overrides the log message when the kind's
	//! spec name is too coarse for the situation.
	void emitTrap(WASM::TrapKind kind, const char* detail);
	//! Emit a trap of `kind` when `pointer` is null (a null reference reached an
	//! instruction that dereferences it). The kind names *what* was dereferenced:
	//! NullReference, NullFunctionReference, NullStructureReference,
	//! NullArrayReference.
	void emitTrapIfNull(WASM::TrapKind kind, jit_value_t pointer, const char* detail = nullptr);
	//! Result storage types implied by block type `bt`.
	std::vector<WASM::StorageType> storageTypesForBlockType(const WASM::BlockType& bt) const;

	//! The `VMContext*` argument of the function being compiled.
	jit_value_t vmContextValue();
	//! Bounds-check `index` against `table` and return it (trap with `kind` on failure).
	jit_value_t checkedTableIndex(jit_value_t table, jit_value_t index, const char* opname,
			WASM::TrapKind kind = WASM::TrapKind::UndefinedElement);
	//! Emits code yielding the LinearMemory* for a (constant) memory index.
	jit_value_t memoryPointerForIndex(WASM::MemIdx memidx);
	//! The declared index type (i32/i64) of memory `memidx`, for the address
	//! widening in effectiveMemoryAddress.
	WASM::MemoryIndexType memoryIndexTypeForIndex(WASM::MemIdx memidx) const;
	//! True when table `tableidx` declares a 64-bit index (flag bit 2).
	bool tableIs64Bit(WASM::TableIdx tableidx) const;

	//! Emits code yielding the TableInstance* for a (constant) table index.
	jit_value_t tablePointerForIndex(WASM::TableIdx tableidx);
	//! Emits a runtime check that `callablePtr`'s canonical type id matches the
	//! module-local type index `typeIdx` (id fast path, registry fallback).
	void emitCallableTypeCheck(jit_value_t callablePtr, WASM::TypeIdx typeIdx);
	//! A lowering the backend cannot perform (in practice: an unmodelled value
	//! type, docs/UNMODELLED_VALUE_TYPES.md). `notImplemented()` throws, which
	//! surfaces as a clean instantiation failure; the LibJIT build region in
	//! ModuleCompiler::compileFunctions is RAII-guarded (docs/SAFETY.md).
	[[noreturn]] void notImplemented(const char* name);

	//! Effective address for `ma` with access size `accessSize` (bounds-checked).
	jit_value_t effectiveMemoryAddress(const WASM::MemArg& ma, jit_nint accessSize);

	//! \par 0xFE atomics (docs/proposals/THREADS.md TH-1)
	//! The shared helpers in WasmBase (wasm_atomic.hpp) perform the access with
	//! `__atomic_*` at seq_cst and trap on a misaligned effective address; these
	//! emit the call and adapt the value types. `effectiveMemoryAddress` has
	//! already bounds-checked the address (and trapped on out-of-bounds).
	jit_value_t emitAtomicLoad(const WASM::MemArg& ma, uint32_t width, jit_type_t resultType);
	//! Atomic store of the top stack value through `ma` (`width` bytes).
	void        emitAtomicStore(const WASM::MemArg& ma, uint32_t width);
	//! Atomic RMW of the top stack value through `ma`; pushes the previous value.
	jit_value_t emitAtomicRmw(const WASM::MemArg& ma, uint32_t width, uint32_t op,
							  jit_type_t resultType);
	//! Atomic compare-exchange through `ma`; pops expected/replacement, pushes old.
	jit_value_t emitAtomicCmpxchg(const WASM::MemArg& ma, uint32_t width,
							  jit_type_t resultType);
	//! Atomic load from a precomputed `address` (shared-memory / GC paths).
	jit_value_t emitAtomicLoadAddress(jit_value_t address, uint32_t width,
									  uint8_t ordering, jit_type_t resultType);
	//! Atomic store of `value` to a precomputed `address`.
	void        emitAtomicStoreRaw(jit_value_t address, jit_value_t value,
								   uint32_t width, uint8_t ordering);
	//! Atomic RMW of `value` at a precomputed `address`; returns the previous value.
	jit_value_t emitAtomicRmwRaw(jit_value_t address, jit_value_t value,
								 uint32_t width, uint32_t op, uint8_t ordering,
								 jit_type_t resultType);
	//! Atomic compare-exchange at a precomputed `address`.
	jit_value_t emitAtomicCmpxchgRaw(jit_value_t address, jit_value_t expected,
									 jit_value_t replacement, uint32_t width,
									 uint8_t ordering, jit_type_t resultType);
	//! Atomic store of the top stack value to a precomputed `address`.
	void        emitAtomicStoreAddress(jit_value_t address, uint32_t width,
									   uint8_t ordering);
	//! Atomic RMW of the top stack value at a precomputed `address`.
	jit_value_t emitAtomicRmwAddress(jit_value_t address, uint32_t width,
									 uint32_t op, uint8_t ordering,
									 jit_type_t resultType);
	//! Atomic compare-exchange at a precomputed `address` (pops expected/replacement).
	jit_value_t emitAtomicCmpxchgAddress(jit_value_t address, uint32_t width,
										 uint8_t ordering, jit_type_t resultType);
	//! Address of the atomic slot for an element of table `tableIdx`.
	jit_value_t tableAtomicSlotAddress(WASM::TableIdx tableIdx);
	//! Address of atomic struct field `fieldIdx` of type `typeIdx`.
	jit_value_t structAtomicFieldAddress(WASM::TypeIdx typeIdx, uint32_t fieldIdx);
	//! Address of an atomic array element of type `typeIdx` (index on the stack).
	jit_value_t arrayAtomicElementAddress(WASM::TypeIdx typeIdx);
	//! Call native helper `fptr` with the given signature and arguments.
	jit_value_t callNativeHelper(void* fptr, jit_type_t resultType,
								 std::vector<jit_type_t> paramTypes,
								 std::vector<jit_value_t> args);
	//! Unsigned 32-bit constant used as an atomic ordering / RMW opcode argument.
	jit_value_t atomicUintConstant(uint32_t value);

	//! \par Exceptions (docs/proposals/EXCEPTIONS.md EX-3/EX-4)
	//! Handlers are resolved statically: `throw` knows its tag at compile time,
	//! `throw_ref` emits a comparison chain. With no handler in this function
	//! the exception becomes *pending* and the function returns zeroes; every
	//! call site checks the pending slot (EXD1).
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
	//! Emits the static dispatch chain for `exn` over the enclosing try_tables;
	//! when no clause matches, sets the pending slot and returns zeroes. All
	//! emitted paths leave the current block.
	void emitExceptionPropagation(jit_value_t exn);
	jit_value_t emitPendingException();
	//! Used after every call: dispatches a pending exception locally or
	//! propagates it, leaving the continuation reachable when none is pending.
/**
 * \brief Emits LibJIT IR for PendingExceptionCheck.
 */
	void emitPendingExceptionCheck();
	//! Returns zeroes of the current function's result types (the ABI still
	//! requires a well-formed return when an exception propagates).
/**
 * \brief Emits LibJIT IR for ReturnZeroes.
 */
	void emitReturnZeroes();

	//! \par Trap status (docs/TRAP_CLEANUP.md §8.5)
	//! A trap is *reported*, not thrown or jumped: generated code calls
	//! `wasm_trap_report` and branches to this function's error epilogue, which
	//! returns zeroes. Every call that can report is followed by a pending check.
	//! No unwind table and no `longjmp` is involved, so a helper may hold a
	//! `std::string` across the trap point.
/**
 * \brief Emits LibJIT IR for PendingTrapCheck.
 */
	void emitPendingTrapCheck();
	void emitReportTrap(WASM::TrapKind kind, const char* detail);
/**
 * \brief Performs the requestTrapEpilogue operation required by the opcode dispatcher.
 */
	void requestTrapEpilogue();
	jit_label_t trapEpilogueLabel = jit_label_undefined; //!< Field `trapEpilogueLabel` of type.
	bool trapEpilogueUsed = false; //!< Field `trapEpilogueUsed` of type.
	//! The calling thread's trap slot, obtained once per function in the entry
	//! block (`wasm_trap_slot()`); every check loads `pending` through it.
	jit_value_t trapSlot = nullptr;
	//! The same for the pending Wasm exception (`wasm_exn_slot()`): a `throw`
	//! leaves a pointer there, and every call site checks it.
	jit_value_t exceptionSlot = nullptr;
	//! Set while the epilogue itself emits helper calls: a failure there must not
	//! branch back into the epilogue.
	bool suppressTrapChecks = false;
	//! True when the active profile excludes the `N` marker: every scalar float
	//! result is canonicalised at an observability point (return, store,
	//! reinterpret, global.set). See docs/proposals/PROFILES.md PR-3.
	const bool deterministicFloats;

	WASM::GlobalType globalTypeForIndex(WASM::GlobalIdx idx) const;

	//! Deterministic-profile float canonicalisation (PROFILES.md PR-3): returns
	//! `value` unchanged unless `deterministicFloats` is set and the value is a
	//! NaN, in which case a canonical NaN of the same width is returned.
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

	//! Call after readCode() to emit return from the implicit function body (Wasm validation
	//! guarantees the value stack matches currentFunc.results).
/**
 * \brief Emits LibJIT IR for ImplicitFunctionReturn.
 */
	void emitImplicitFunctionReturn();

	//! Emits the function prologue: the guest call-stack guard and the pending
	//! check that turns a guard trap into an early return. Call before readCode().
/**
 * \brief Emits LibJIT IR for FunctionEntry.
 */
	void emitFunctionEntry();

	//! Emits the shared error epilogue (return zeroes) when any trap path was
	//! emitted. Call after emitImplicitFunctionReturn().
/**
 * \brief Emits LibJIT IR for TrapEpilogue.
 */
	void emitTrapEpilogue();

	//! OpcodeDispatcher interface
protected:
	//! `internals.translatedTypes` is parallel to `module.typeIds` (filled in translateTypes).
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

/**
 * \brief Dispatches the Unreachable WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchUnreachable() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the Nop WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchNop() override; //!< Lower this opcode into the backend IR.
	void dispatchBlock(const WASM::BlockType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchLoop(const WASM::BlockType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchIf(const WASM::BlockType& arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the Else WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchElse() override; //!< Lower this opcode into the backend IR.
	void dispatchThrow(WASM::TagIdx arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the ThrowRef WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchThrowRef() override; //!< Lower this opcode into the backend IR.
	void dispatchContNew(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchContBind(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchSuspend(WASM::TagIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchResume(WASM::TypeIdx arg, std::vector<WASM::ContHandlerClause>&& handlers) override; //!< Lower this opcode into the backend IR.
	//! Dispatch the `ResumeThrow` instruction. Lower this opcode into the backend IR.
	void dispatchResumeThrow(WASM::TypeIdx arg1, WASM::TagIdx arg2,
			std::vector<WASM::ContHandlerClause>&& handlers) override; //!< Field `override` of type.
	//! Dispatch the `ResumeThrowRef` instruction. Lower this opcode into the backend IR.
	void dispatchResumeThrowRef(WASM::TypeIdx arg,
			std::vector<WASM::ContHandlerClause>&& handlers) override; //!< Field `override` of type.
	void dispatchSwitch(WASM::TypeIdx arg1, WASM::TagIdx arg2) override; //!< Lower this opcode into the backend IR.
	void emitContinuationResultHandling(const WASM::FuncType& func,
			const std::vector<WASM::ContHandlerClause>& handlers,
			jit_value_t resultAddr, jit_value_t resultBuffer,
			jit_value_t tagBuffer, size_t baseDepth);
/**
 * \brief Dispatches the End WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchEnd() override; //!< Lower this opcode into the backend IR.
	void dispatchBr(WASM::LabelIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchBrIf(WASM::LabelIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the Return WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchReturn() override; //!< Lower this opcode into the backend IR.
	void dispatchCall(WASM::FuncIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchReturnCall(WASM::FuncIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchCallRef(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchReturnCallRef(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the Drop WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchDrop() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the Select WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchSelect() override; //!< Lower this opcode into the backend IR.
	void dispatchSelectT(std::vector<WASM::ValueType>&& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchLocalGet(WASM::LocalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchLocalSet(WASM::LocalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchLocalTee(WASM::LocalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalGet(WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalSet(WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableGet(WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableSet(WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Load(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Load(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchF32Load(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchF64Load(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Load8S(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Load8U(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Load16S(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Load16U(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Load8S(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Load8U(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Load16S(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Load16U(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Load32S(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Load32U(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Store(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Store(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchF32Store(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchF64Store(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Store8(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Store16(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Store8(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Store16(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Store32(WASM::MemArg addr) override; //!< Lower this opcode into the backend IR.
	void dispatchMemorySize(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryGrow(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32Const(int32_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64Const(int64_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchF32Const(float arg) override; //!< Lower this opcode into the backend IR.
	void dispatchF64Const(double arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Eqz WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Eqz() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32LtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32LtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32LtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32LtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32GtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32GtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32GtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32GtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32LeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32LeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32LeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32LeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32GeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32GeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32GeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32GeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Eqz WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Eqz() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64LtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64LtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64LtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64LtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64GtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64GtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64GtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64GtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64LeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64LeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64LeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64LeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64GeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64GeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64GeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64GeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Lt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Lt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Gt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Gt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Le WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Le() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Ge WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Ge() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Lt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Lt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Gt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Gt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Le WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Le() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Ge WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Ge() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Clz WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Clz() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Ctz WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Ctz() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Popcnt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Popcnt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32DivS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32DivS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32DivU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32DivU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32RemS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32RemS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32RemU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32RemU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32And WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32And() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Or WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Or() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Xor WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Xor() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Shl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Shl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32ShrS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32ShrS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32ShrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32ShrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Rotl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Rotl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Rotr WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Rotr() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Clz WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Clz() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Ctz WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Ctz() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Popcnt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Popcnt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64DivS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64DivS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64DivU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64DivU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64RemS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64RemS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64RemU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64RemU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64And WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64And() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Or WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Or() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Xor WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Xor() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Shl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Shl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64ShrS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64ShrS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64ShrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64ShrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Rotl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Rotl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Rotr WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Rotr() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Ceil WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Ceil() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Floor WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Floor() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Trunc WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Trunc() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Nearest WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Nearest() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Sqrt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Sqrt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Div WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Div() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Min WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Min() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Max WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Max() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32Copysign WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32Copysign() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Ceil WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Ceil() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Floor WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Floor() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Trunc WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Trunc() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Nearest WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Nearest() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Sqrt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Sqrt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Div WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Div() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Min WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Min() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Max WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Max() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64Copysign WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64Copysign() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32WrapI64 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32WrapI64() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncF32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncF32S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncF32U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncF32U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncF64S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncF64S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncF64U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncF64U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64ExtendI32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64ExtendI32S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64ExtendI32U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64ExtendI32U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncF32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncF32S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncF32U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncF32U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncF64S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncF64S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncF64U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncF64U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32ConvertI32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32ConvertI32S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32ConvertI32U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32ConvertI32U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32ConvertI64S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32ConvertI64S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32ConvertI64U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32ConvertI64U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32DemoteF64 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32DemoteF64() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64ConvertI32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64ConvertI32S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64ConvertI32U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64ConvertI32U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64ConvertI64S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64ConvertI64S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64ConvertI64U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64ConvertI64U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64PromoteF32 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64PromoteF32() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32ReinterpretF32 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32ReinterpretF32() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64ReinterpretF64 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64ReinterpretF64() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32ReinterpretI32 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32ReinterpretI32() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64ReinterpretI64 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64ReinterpretI64() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Extend8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Extend8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32Extend16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32Extend16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Extend8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Extend8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Extend16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Extend16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Extend32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Extend32S() override; //!< Lower this opcode into the backend IR.
	void dispatchRefNull(const WASM::HeapType& arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the RefIsNull WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchRefIsNull() override; //!< Lower this opcode into the backend IR.
	void dispatchRefFunc(WASM::FuncIdx arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the RefEq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchRefEq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the RefAsNonNull WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchRefAsNonNull() override; //!< Lower this opcode into the backend IR.
	void dispatchBrOnNull(WASM::LabelIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchBrOnNonNull(WASM::LabelIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStructNew(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStructNewDefault(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	//! Custom Descriptors (0xFB 32..38).
	void dispatchStructNewDesc(WASM::TypeIdx arg) override;
	void dispatchStructNewDefaultDesc(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchRefGetDesc(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchRefCastDescEq(const WASM::HeapType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchRefCastDescEqNull(const WASM::HeapType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchBrOnCastDescEq(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Lower this opcode into the backend IR.
	void dispatchBrOnCastDescEqFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayNew(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayNewDefault(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	//! More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2).
	void dispatchArrayNewArray(WASM::TypeIdx dst, WASM::TypeIdx src) override;
	void dispatchArrayNewMemory(WASM::TypeIdx dst, uint32_t src) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayNewTable(WASM::TypeIdx dst, uint32_t src) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayGet(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayGetS(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayGetU(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArraySet(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the ArrayLen WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchArrayLen() override; //!< Lower this opcode into the backend IR.
	void dispatchArrayFill(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchRefTest(const WASM::HeapType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchRefTestNull(const WASM::HeapType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchRefCast(const WASM::HeapType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchRefCastNull(const WASM::HeapType& arg) override; //!< Lower this opcode into the backend IR.
	void dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Lower this opcode into the backend IR.
	void dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the AnyConvertExtern WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchAnyConvertExtern() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the ExternConvertAny WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchExternConvertAny() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the RefI31 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchRefI31() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I31GetS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI31GetS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I31GetU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI31GetU() override; //!< Lower this opcode into the backend IR.

	//! Stringref core instructions (docs/proposals/STRINGREF.md STR-3).
	void dispatchStringNewUtf8(WASM::MemIdx arg) override;
	void dispatchStringNewWtf16(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringNewLossyUtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringNewWtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringConst(uint32_t idx) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringMeasureUtf8 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringMeasureUtf8() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringMeasureWtf8 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringMeasureWtf8() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringMeasureWtf16 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringMeasureWtf16() override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeUtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeWtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeWtf16(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringConcat WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringConcat() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringEq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringEq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringIsUsvSequence WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringIsUsvSequence() override; //!< Lower this opcode into the backend IR.

	//! stringview facility (docs/proposals/STRINGREF.md STR-4)
/**
 * \brief Dispatches the StringAsWtf8 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringAsWtf8() override;
/**
 * \brief Dispatches the StringViewWtf8Advance WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewWtf8Advance() override; //!< Lower this opcode into the backend IR.
	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewWtf8Slice WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewWtf8Slice() override; //!< Lower this opcode into the backend IR.
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringAsWtf16 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringAsWtf16() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewWtf16Length WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewWtf16Length() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewWtf16GetCodeUnit WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewWtf16GetCodeUnit() override; //!< Lower this opcode into the backend IR.
	void dispatchStringViewWtf16Encode(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewWtf16Slice WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewWtf16Slice() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringAsIter WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringAsIter() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewIterNext WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewIterNext() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewIterAdvance WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewIterAdvance() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewIterRewind WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewIterRewind() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the StringViewIterSlice WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchStringViewIterSlice() override; //!< Lower this opcode into the backend IR.

	//! GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
	void dispatchStringNewUtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringNewWtf16Array(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringNewWtf8Array(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStringNewArrayImpl(WASM::TypeIdx arg, void* helper, int mode); //!< Lower this opcode into the backend IR.
	void dispatchStringEncodeArrayImpl(WASM::TypeIdx arg, void* helper, int mode); //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncSatF32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncSatF32S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncSatF32U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncSatF32U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncSatF64S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncSatF64S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32TruncSatF64U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32TruncSatF64U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncSatF32S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncSatF32S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncSatF32U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncSatF32U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncSatF64S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncSatF64S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64TruncSatF64U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64TruncSatF64U() override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchDataDrop(uint32_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryFill(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryDiscard(WASM::MemIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchRoundingVariant(uint32_t sub) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Add128 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Add128() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64Sub128 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64Sub128() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64MulWideS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64MulWideS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64MulWideU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64MulWideU() override; //!< Lower this opcode into the backend IR.
	void dispatchI32Itt(uint32_t immediate) override; //!< Lower this opcode into the backend IR.
	void dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchElemDrop(uint32_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchTableGrow(WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableSize(WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableFill(WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load8x8S(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load8x8U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load16x4S(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load16x4U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load32x2S(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load32x2U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load8Splat(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load16Splat(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load32Splat(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load64Splat(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load32Zero(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load64Zero(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Store(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchV128Const(std::span<uint8_t> arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Swizzle WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Swizzle() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Splat WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Splat() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Splat WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Splat() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Splat WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Splat() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Splat WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Splat() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Splat WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Splat() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Splat WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Splat() override; //!< Lower this opcode into the backend IR.
	void dispatchI8x16ExtractLaneS(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI8x16ExtractLaneU(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI8x16ReplaceLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI16x8ExtractLaneS(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI16x8ExtractLaneU(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI16x8ReplaceLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32x4ExtractLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32x4ReplaceLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64x2ExtractLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64x2ReplaceLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchF32x4ExtractLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchF32x4ReplaceLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchF64x2ExtractLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
	void dispatchF64x2ReplaceLane(uint8_t arg) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16LtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16LtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16LtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16LtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16GtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16GtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16GtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16GtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16LeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16LeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16LeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16LeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16GeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16GeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16GeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16GeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8LtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8LtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8LtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8LtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8GtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8GtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8GtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8GtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8LeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8LeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8LeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8LeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8GeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8GeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8GeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8GeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4LtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4LtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4LtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4LtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4GtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4GtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4GtU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4GtU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4LeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4LeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4LeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4LeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4GeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4GeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4GeU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4GeU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Lt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Lt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Gt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Gt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Le WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Le() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Ge WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Ge() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Lt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Lt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Gt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Gt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Le WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Le() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Ge WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Ge() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the V128Not WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchV128Not() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the V128And WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchV128And() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the V128AndNot WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchV128AndNot() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the V128Or WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchV128Or() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the V128Xor WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchV128Xor() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the V128Bitselect WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchV128Bitselect() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the V128AnyTrue WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchV128AnyTrue() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Popcnt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Popcnt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16AllTrue WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16AllTrue() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Bitmask WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Bitmask() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16NarrowI16x8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16NarrowI16x8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16NarrowI16x8U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16NarrowI16x8U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Shl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Shl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16ShrS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16ShrS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16ShrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16ShrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16AddSatS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16AddSatS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16AddSatU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16AddSatU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16SubSatS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16SubSatS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16SubSatU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16SubSatU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16MinS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16MinS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16MinU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16MinU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16MaxS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16MaxS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16MaxU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16MaxU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16AvgrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16AvgrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtAddPairwiseI8x16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtAddPairwiseI8x16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtAddPairwiseI8x16U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtAddPairwiseI8x16U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Q15MulRSatS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Q15MulRSatS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8AllTrue WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8AllTrue() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Bitmask WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Bitmask() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8NarrowI32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8NarrowI32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8NarrowI32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8NarrowI32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtendLowI8x16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtendLowI8x16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtendHighI8x16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtendHighI8x16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtendLowI8x16U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtendLowI8x16U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtendHighI8x16U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtendHighI8x16U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Shl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Shl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ShrS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ShrS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ShrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ShrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8AddSatS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8AddSatS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8AddSatU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8AddSatU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8SubSatS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8SubSatS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8SubSatU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8SubSatU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8MinS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8MinS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8MinU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8MinU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8MaxS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8MaxS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8MaxU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8MaxU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8AvgrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8AvgrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtMulLowI8x16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtMulLowI8x16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtMulHighI8x16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtMulHighI8x16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtMulLowI8x16U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtMulLowI8x16U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8ExtMulHighI8x16U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8ExtMulHighI8x16U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtAddPairwiseI16x8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtAddPairwiseI16x8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtAddPairwiseI16x8U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtAddPairwiseI16x8U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4AllTrue WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4AllTrue() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Bitmask WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Bitmask() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtendLowI16x8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtendLowI16x8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtendHighI16x8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtendHighI16x8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtendLowI16x8U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtendLowI16x8U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtendHighI16x8U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtendHighI16x8U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Shl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Shl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ShrS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ShrS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ShrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ShrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4MinS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4MinS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4MinU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4MinU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4MaxS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4MaxS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4MaxU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4MaxU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4DotI16x8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4DotI16x8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtMulLowI16x8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtMulLowI16x8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtMulHighI16x8S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtMulHighI16x8S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtMulLowI16x8U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtMulLowI16x8U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4ExtMulHighI16x8U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4ExtMulHighI16x8U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2AllTrue WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2AllTrue() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Bitmask WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Bitmask() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtendLowI32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtendLowI32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtendHighI32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtendHighI32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtendLowI32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtendLowI32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtendHighI32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtendHighI32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Shl WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Shl() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ShrS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ShrS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ShrU WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ShrU() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Eq WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Eq() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2Ne WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2Ne() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2LtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2LtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2GtS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2GtS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2LeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2LeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2GeS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2GeS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtMulLowI32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtMulLowI32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtMulHighI32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtMulHighI32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtMulLowI32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtMulLowI32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2ExtMulHighI32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2ExtMulHighI32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Ceil WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Ceil() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Floor WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Floor() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Trunc WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Trunc() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Nearest WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Nearest() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Sqrt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Sqrt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Div WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Div() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Min WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Min() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4Max WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4Max() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4PMin WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4PMin() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4PMax WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4PMax() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Ceil WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Ceil() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Floor WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Floor() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Trunc WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Trunc() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Nearest WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Nearest() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Abs WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Abs() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Neg WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Neg() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Sqrt WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Sqrt() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Add WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Add() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Sub WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Sub() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Mul WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Mul() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Div WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Div() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Min WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Min() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2Max WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2Max() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2PMin WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2PMin() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2PMax WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2PMax() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4TruncSatF32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4TruncSatF32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4TruncSatF32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4TruncSatF32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4ConvertI32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4ConvertI32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4ConvertI32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4ConvertI32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4TruncSatF64x2SZero WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4TruncSatF64x2SZero() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4TruncSatF64x2UZero WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4TruncSatF64x2UZero() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2ConvertLowI32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2ConvertLowI32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2ConvertLowI32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2ConvertLowI32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4DemoteF64x2Zero WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4DemoteF64x2Zero() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2PromoteLowF32x4 WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2PromoteLowF32x4() override; //!< Lower this opcode into the backend IR.

	//! Relaxed SIMD (0x100-0x113): SD5 picks the strict result for each.
/**
 * \brief Dispatches the I8x16RelaxedSwizzle WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16RelaxedSwizzle() override;
/**
 * \brief Dispatches the I32x4RelaxedTruncF32x4S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4RelaxedTruncF32x4S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4RelaxedTruncF32x4U WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4RelaxedTruncF32x4U() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4RelaxedTruncF64x2SZero WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4RelaxedTruncF64x2SZero() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4RelaxedTruncF64x2UZero WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4RelaxedTruncF64x2UZero() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4RelaxedMadd WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4RelaxedMadd() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4RelaxedNmadd WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4RelaxedNmadd() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2RelaxedMadd WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2RelaxedMadd() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2RelaxedNmadd WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2RelaxedNmadd() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I8x16RelaxedLaneselect WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI8x16RelaxedLaneselect() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8RelaxedLaneselect WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8RelaxedLaneselect() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4RelaxedLaneselect WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4RelaxedLaneselect() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I64x2RelaxedLaneselect WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI64x2RelaxedLaneselect() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4RelaxedMin WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4RelaxedMin() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F32x4RelaxedMax WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF32x4RelaxedMax() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2RelaxedMin WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2RelaxedMin() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the F64x2RelaxedMax WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchF64x2RelaxedMax() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8RelaxedQ15MulRS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8RelaxedQ15MulRS() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I16x8RelaxedDotI8x16I7x16S WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI16x8RelaxedDotI8x16I7x16S() override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the I32x4RelaxedDotI8x16I7x16AddS WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryAtomicNotify(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryAtomicWait32(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchMemoryAtomicWait64(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchAtomicFence(uint8_t ordering) override; //!< Lower this opcode into the backend IR.
/**
 * \brief Dispatches the AtomicPause WebAssembly instruction to this LibJIT implementation.
 */
	void dispatchAtomicPause() override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicLoad(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicLoad(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicStore(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicStore(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicStore8(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicStore16(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicStore8(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicStore16(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicStore32(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override; //!< Lower this opcode into the backend IR.
};

}

#endif // LIBJITOPCODEDISPATCHER_HPP
