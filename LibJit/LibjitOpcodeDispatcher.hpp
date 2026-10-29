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
	//! \param v the jit_value_t operand
	void pushValue(jit_value_t v);
	//! Push a reference whose ownership the stack takes over (no clone).
	//! \param v the jit_value_t operand
	void pushOwnedValue(jit_value_t v);
	//! Release the operand stack's ownership of a popped reference.
	//! \param v the jit_value_t operand
	void emitDropValue(jit_value_t v);
	//! Release every reference-typed non-parameter local (function exit).
	void emitDropRefLocals();
	//! Pop and return the top of the model operand stack (placeholder when unreachable).
	//! \return the top of the model operand stack; in unreachable code a
	//! placeholder value, so that lowering never underflows
	jit_value_t popValue();
	//! Emits the function's epilogue (a `return`). Does nothing in unreachable
	//! code or when the model stack is short (dead tail). With `consume` false
	//! the return operands are left on the model stack (for a conditional
	//! branch to the function's implicit block, whose fallthrough needs them).
	//! \param consume the flag the instruction carries
	void emitFunctionReturn(bool consume = true);
	//! Emits a native tail call (LibJIT `JIT_CALL_TAIL`) when the target is this
	//! very function — the only case this LibJIT build lowers reliably (see
	//! docs/TAILCALLS.md §6). Returns false when the caller must fall back to
	//! `call` + `return`.
	//! \param funcIdx the module-local function index the call targets
	//! \return true when the native tail call was emitted, false when the caller
	//! must fall back to `call` plus `return`
	bool emitDirectTailCall(WASM::FuncIdx funcIdx);
	//! True when `calleeSig` has exactly this function's parameter and result
	//! types. The trampoline no longer needs it (TC-3 dispatches through the
	//! callee type's thunk), but it is part of the shipped LibJit symbol set, so
	//! it stays for the additive-only ABI rule (docs/API_ABI_STABILITY.md G2).
	//! \param calleeSig the callee signature to compare with this function
	//! \return true when `calleeSig` has exactly this function’s parameter and
	//! result types, false otherwise
	bool signatureIsUniform(const WASM::FuncType& calleeSig) const;
	//! Publishes {target, args, pending} to the current execution state's
	//! trampoline state and returns a dummy value; the driver re-dispatches
	//! through the target type's thunk (docs/TAILCALLS.md TC-2/TC-3). Callers
	//! must have verified WASM::tailCallSlotsSupported() and popped `args`.
	//! \param targetCallablePtr the jit_value_t operand
	//! \param args the std::vector<jit_value_t> operand
	void emitTailCallViaPending(jit_value_t targetCallablePtr, const std::vector<jit_value_t>& args);
	//! Semantic fallback for a tail call the trampoline cannot take: a
	//! `call` + `return` with the callee's results becoming this function's.
	//! Used for a native import on the direct path and for a callable with no
	//! dispatch thunk on the indirect/ref paths.
	//! \param typeIdx the module-local type index of the callee signature
	//! \param callablePtr the callable pointer of the callee
	//! \param args the argument values, in declaration order
	void emitTailCallFallback(WASM::TypeIdx typeIdx, jit_value_t callablePtr,
							  const std::vector<jit_value_t>& args);
	//! Emits `return` of a zero value of this function's result type.
	void emitDummyReturn();
	//! Emits the jump for a branch target; a target past the control stack is
	//! the function's implicit block (i.e. a return).
	//! \param arg the label index, a depth relative to the current block
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
	//! \return the hint whose offset matches the instruction being dispatched, or
	//! nullptr when the module carries no hint for it
	const WASM::BranchHint* currentBranchHint() const;
	//! A zero (or null) constant of LibJIT type `t`, used as a placeholder or trap return.
	//! \param t the LibJIT type whose zero value is wanted
	//! \return a constant zero (a null pointer for a reference type) of type `t`
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
	//! \return the memoised LibJIT struct type whose 16 bytes hold a `v128`
	jit_type_t v128Type();
	//! A fresh 16-byte slot for a `v128` result.
	//! \return a fresh frame local that can hold one `v128` operand
	jit_value_t newV128Slot();
	//! Address of a global's 16 bytes in the VMContext's `globals` array. The
	//! `v128` member sits at offset 0 of the `Value` union, so this is also the
	//! pointer the load/store helpers accept (SIMD-L5).
	//! \param arg the global index
	//! \return the address of that global’s 16 bytes inside the VMContext
	jit_value_t globalSlotAddress(WASM::GlobalIdx arg);
	//! Calls a helper through a memoised signature (§6.4) and returns its result
	//! (`void` for the out-slot shapes — the caller then pushes the slot).
	//! \param fptr the address of the native helper to call
	//! \param ret the return type of the helper (`jit_type_void` for the
	//! out-slot shapes)
	//! \param params the parameter types of the helper, used as the signature key
	//! \param args the argument values to pass, in declaration order
	//! \return the helper’s result value, or a null value for a void helper
	jit_value_t callSimdHelper(void* fptr, jit_type_t ret,
			std::span<const jit_type_t> params, std::span<const jit_value_t> args);
	//! The common shape: leading operands, then the result slot's address.
	//! \param fptr the address of the native helper to call
	//! \param out the address of the slot the helper writes its result into
	//! \param leadingTypes the parameter types that precede the out pointer
	//! \param leadingArgs the argument values that precede the out pointer
	//! \return the call’s (void) result value; the caller pushes `out`
	jit_value_t callSimdOutHelper(void* fptr, jit_value_t out,
			std::span<const jit_type_t> leadingTypes, std::span<const jit_value_t> leadingArgs);
	//! The `v128.load*` / `v128.store*` shapes. The effective address is computed
	//! and bounds-checked by `effectiveMemoryAddress` — the same path the scalar
	//! loads take, trap included — so the helpers themselves never trap and need
	//! no context (docs/SIMD_LIBJIT.md §6.3). Operand order is wasm's: for a lane
	//! or store form the vector is on top, the address below it.
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the jit_nint operand
	//! \param helper the void* operand
	void emitSimdLoad(const WASM::MemArg& ma, jit_nint width, void* helper);
	//! `v128.load*_lane`: load `width` bytes into lane `lane` of the vector on top.
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the jit_nint operand
	//! \param lane the 8-bit immediate of the instruction
	//! \param helper the void* operand
	void emitSimdLoadLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper);
	//! `v128.store*`: store the top vector's low `width` bytes to memory.
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the jit_nint operand
	//! \param helper the void* operand
	void emitSimdStore(const WASM::MemArg& ma, jit_nint width, void* helper);
	//! `v128.store*_lane`: store one lane of the top vector to memory.
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the jit_nint operand
	//! \param lane the 8-bit immediate of the instruction
	//! \param helper the void* operand
	void emitSimdStoreLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper);
	//! The per-lane shapes: two vectors in and one out (used for the arithmetic
	//! *and* for the mask-producing comparisons — the difference is entirely in
	//! the helper), one vector in and one out, and the three-operand bitselect.
	//! \param helper the void* operand
	void emitSimdBinaryHelper(void* helper);
	//! Unary SIMD helper: one vector in, one vector out.
	//! \param helper the void* operand
	void emitSimdUnaryHelper(void* helper);
	//! Ternary SIMD helper (e.g. `v128.bitselect`): three vectors in, one out.
	//! \param helper the void* operand
	void emitSimdTernaryHelper(void* helper);
	//! A shift takes the vector plus a scalar count; a reduction yields a wasm i32
	//! rather than a vector, so it has no out slot at all.
	//! \param helper the void* operand
	void emitSimdShiftHelper(void* helper);
	//! SIMD reduction to i32 (no out-slot): pop a vector, push the helper's i32.
	//! \param helper the void* operand
	void emitSimdReduceI32Helper(void* helper);
	//! Signature cache: the SIMD ops share a handful of shapes, so memoise them
	//! instead of creating (and leaking) one signature per instruction (§6.4).
	std::vector<std::pair<std::vector<jit_type_t>, jit_type_t>> simdSignatureCache;
	//! Memoised LibJIT struct type for `v128` (null until first use).
	jit_type_t simdCachedType = nullptr;
	//! Reinterpret / cast a reference-typed value to `targetType` without changing bits.
	//! \param value the reference-typed value to retype
	//! \param targetType the LibJIT reference type to view it as
	//! \return the same bits, typed as `targetType`
	jit_value_t castRefValue(jit_value_t value, jit_type_t targetType);
	//! Narrow a reference-typed value to `void*` for helper and store calls.
	//! \param value the reference-typed value to narrow
	//! \return the same bits, typed as `void*` for a helper or store call
	jit_value_t refAsVoidPtr(jit_value_t value);
	//! Emits the reference-counting write barrier (`wasm_ref_store`): retain
	//! `newRef`, store it at `slotAddr`, release the value it overwrote. Used
	//! for every reference-typed slot store (docs/proposals/GC.md D15/RC-1).
	//! \param slotAddr the jit_value_t operand
	//! \param newRef the jit_value_t operand
	void emitRefStore(jit_value_t slotAddr, jit_value_t newRef);
	//! `wasm_ref_clone` / `wasm_ref_drop` (retain / release), for slot stores
	//! that address a LibJIT local rather than memory (RC-5).
	//! \param ref the reference whose ownership count is raised
	//! \return `ref` itself, now owning one more count
	jit_value_t emitRefClone(jit_value_t ref);
	//! Call `wasm_ref_drop` on `ref` (release one ownership count).
	//! \param ref the jit_value_t operand
	void emitRefDrop(jit_value_t ref);
	//! A typed null reference constant of LibJIT reference type `refType`.
	//! \param refType the LibJIT reference type of the constant
	//! \return a null reference constant of that type
	jit_value_t typedNullRef(jit_type_t refType);
	//! Emit `ref.test` / nullability check of `ref` against `heapType`.
	//! \param ref the reference value to test
	//! \param heapType the heap type the reference is tested against
	//! \param nullable true for the `ref.test (ref null ht)` form, which also
	//! accepts a null reference
	//! \return a 32-bit value that is non-zero when the test succeeds
	jit_value_t emitRefTypeTest(jit_value_t ref, const WASM::HeapType& heapType, bool nullable);
	//! LibJIT element type of table `arg`.
	//! \param arg the table index
	//! \return the LibJIT type of that table’s elements
	jit_type_t tableElementJitType(WASM::TableIdx arg) const;
	//! LibJIT reference type for heap type `ht` (nullable when `nullable`).
	//! \param ht the heap type to translate
	//! \param nullable true for a `(ref null ht)` type, false for `(ref ht)`
	//! \return the LibJIT reference type that corresponds to the pair
	jit_type_t jitRefTypeForHeapType(const WASM::HeapType& ht, bool nullable);
	//! LibJIT `(ref [null] $struct)` type for type index `arg`.
	//! \param arg the module-local type index of the struct type
	//! \param nullable true for `(ref null $t)`, false for `(ref $t)`
	//! \return the LibJIT reference type for that struct
	jit_type_t structRefType(WASM::TypeIdx arg, bool nullable) const;
	//! LibJIT `(ref [null] $array)` type for type index `arg`.
	//! \param arg the module-local type index of the array type
	//! \param nullable true for `(ref null $t)`, false for `(ref $t)`
	//! \return the LibJIT reference type for that array
	jit_type_t arrayRefType(WASM::TypeIdx arg, bool nullable) const;
	//! Canonical struct type view for module type index `arg`.
	//! \param arg the module-local type index
	//! \return a view of the canonical struct type behind that index
	WASM::CoreTypeView structTypeForIndex(WASM::TypeIdx arg) const;
	//! Canonical array type view for module type index `arg`.
	//! \param arg the module-local type index
	//! \return a view of the canonical array type behind that index
	WASM::CoreTypeView arrayTypeForIndex(WASM::TypeIdx arg) const;
	//! LibJIT type of the element of array type `arg`.
	//! \param arg the module-local type index of the array type
	//! \return the LibJIT type of one element of that array
	jit_type_t arrayElementJitType(WASM::TypeIdx arg) const;
	//! Byte offset of field `fieldIndex` inside struct type `arg`.
	//! \param arg the module-local type index of the struct type
	//! \param fieldIndex the position of the field in the struct
	//! \return the byte offset of that field in the lowered layout
	jit_nint structFieldOffset(WASM::TypeIdx arg, uint32_t fieldIndex) const;
	//! Byte offset of the length word inside an array of type `arg`.
	//! \param arg the module-local type index of the array type
	//! \return the byte offset of the length word in the lowered layout
	jit_nint arrayLengthOffset(WASM::TypeIdx arg) const;
	//! Byte offset of the inline element payload inside an array of type `arg`.
	//! \param arg the module-local type index of the array type
	//! \return the byte offset at which the inline element payload starts
	jit_nint arrayDataOffset(WASM::TypeIdx arg) const;
	//! `ref + dataOffset`, typed as a pointer to the array's element type.
	//! Arrays store their elements inline (docs/proposals/GC.md D13/GC-9), so
	//! there is no `data` field to load.
	//! \param arrayRef the array reference the elements are addressed through
	//! \param arg the module-local type index of the array type
	//! \return a pointer to the first element, typed as the element type
	jit_value_t arrayElementBase(jit_value_t arrayRef, WASM::TypeIdx arg);
	//! Layout of a multi-result function's result tuple. A multi-result
	//! signature carries a trailing `void*` result buffer instead of returning
	//! an aggregate by value (see LibJitTypeTranslation.cpp); this struct type
	//! describes that buffer.
	//! \param wasm_func the function signature whose results are packed
	//! \return the LibJIT struct type that describes the result buffer of that
	//! signature
	jit_type_t resultTupleType(const WASM::FuncType& wasm_func);
	//! Writes the top `resultCount` values into `out`, the caller-provided
	//! result buffer (a multi-result function's trailing parameter).
	//! \param tupleType the jit_type_t operand
	//! \param out the jit_value_t operand
	//! \param resultCount the count of entries
	void packReturnValues(jit_type_t tupleType, jit_value_t out, size_t resultCount);
	//! Pushes a call's results. For a multi-result callee `outBuffer` is the
	//! address of the caller's result tuple and `ret` is unused (the callee
	//! returns void); for 0/1 results `outBuffer` is null.
	//! \param calleeSig the FuncType operand
	//! \param ret the jit_value_t operand
	//! \param outBuffer the jit_value_t operand
	void pushCallResults(const WASM::FuncType& calleeSig, jit_value_t ret, jit_value_t outBuffer);
	//! Allocates the caller-side result tuple in this function's frame and
	//! returns its address, or null when `calleeSig` has 0/1 results.
	//! \param calleeSig the callee signature whose results must be stored
	//! \return the address of the caller-side result tuple, or null when the
	//! callee has zero or one result
	jit_value_t newResultBuffer(const WASM::FuncType& calleeSig);
	//! Parameter storage types implied by block type `bt`.
	//! \param bt the block type to inspect
	//! \return the storage types the construct consumes on entry
	std::vector<WASM::StorageType> parameterTypesForBlockType(const WASM::BlockType& bt) const;
	//! Allocate one frame local per type in `types` (branch / block parameter slots).
	//! \param types the storage types that need a frame slot each
	//! \return one fresh frame local per entry of `types`, in the same order
	std::vector<jit_value_t> createSlotsForTypes(const std::vector<WASM::StorageType>& types);
	//! Pop `|slots|` values from the model stack and store them into `slots`.
	//! \param slots the std::vector<jit_value_t> operand
	void storeStackTopToSlots(const std::vector<jit_value_t>& slots);
	//! Push the values currently held in `slots` onto the model stack.
	//! \param slots the std::vector<jit_value_t> operand
	void restoreValuesFromSlots(const std::vector<jit_value_t>& slots);
	//! Truncate or grow the model operand stack to `newSize` (unreachable placeholders).
	//! \param newSize the count of entries
	void resizeValueStack(size_t newSize);
	//! Control frame targeted by relative label `arg` (must be in range).
	//! \param arg the relative label depth, zero being the innermost block
	//! \return the control frame the label names
	ControlBlock& branchTarget(WASM::LabelIdx arg);
	//! Result (or loop parameter) types a branch to `target` must leave on the stack.
	//! \param target the control frame the branch targets
	//! \return the result types (for a loop: its parameter types) the branch must
	//! leave on the stack
	const std::vector<WASM::StorageType>& branchTypesForTarget(const ControlBlock& target) const;
	//! Frame slots a branch to `target` writes before jumping.
	//! \param target the control frame the branch targets
	//! \return the frame slots the branch writes before jumping
	const std::vector<jit_value_t>& branchSlotsForTarget(const ControlBlock& target) const;
	//! Store branch values into `target`'s slots and jump to its label.
	//! \param target the ControlBlock operand
	void emitBranchToTarget(ControlBlock& target);
	//! Module-local type index of function `funcIdx` (imports first).
	//! \param funcIdx the function index, imports first
	//! \return the module-local type index of that function’s signature
	WASM::TypeIdx functionTypeIndexForFunc(WASM::FuncIdx funcIdx) const;
	//! Function signature for module type index `typeIdx`.
	//! \param typeIdx the module-local type index
	//! \return the function signature stored at that index
	WASM::FuncType functionSignatureForType(WASM::TypeIdx typeIdx) const;
	//! `cont $ft`: resolve the module-local function type index stored in the
	//! canonical continuation type (STACK_SWITCHING.md SS-1).
	//! \param contTypeIdx the module-local type index of the continuation type
	//! \return the module-local function type index that continuation wraps
	WASM::TypeIdx continuationFuncIndex(WASM::TypeIdx contTypeIdx) const;
	//! Function signature wrapped by continuation type `contTypeIdx`.
	//! \param contTypeIdx the module-local type index of the continuation type
	//! \return the function signature the continuation wraps
	WASM::FuncType continuationSignature(WASM::TypeIdx contTypeIdx) const;
	//! Pointer to the callable for function index `funcIdx` (import or local).
	//! \param funcIdx the function index, imports first
	//! \return a pointer value to that function’s callable
	jit_value_t callablePointerForFuncIndex(WASM::FuncIdx funcIdx);
	//! Lowering hook: call through callable.
	//! Lower this opcode into the backend IR.
	//! \param callablePtr the jit_value_t operand
	//! \param typeIdx the type index
	void dispatchCallThroughCallable(jit_value_t callablePtr, WASM::TypeIdx typeIdx);
	//! Emit a trap of `kind` (docs/SAFETY.md §2.4).
	//! \param kind the TrapKind operand
	void emitTrap(WASM::TrapKind kind);
	//! Emit a trap of `kind`; `detail` overrides the log message when the kind's
	//! spec name is too coarse for the situation.
	//! \param kind the TrapKind operand
	//! \param detail the name of the opcode the base class does not implement
	void emitTrap(WASM::TrapKind kind, const char* detail);
	//! Emit a trap of `kind` when `pointer` is null (a null reference reached an
	//! instruction that dereferences it). The kind names *what* was dereferenced:
	//! NullReference, NullFunctionReference, NullStructureReference,
	//! NullArrayReference.
	//! \param kind the TrapKind operand
	//! \param pointer the jit_value_t operand
	//! \param detail the name of the opcode the base class does not implement
	void emitTrapIfNull(WASM::TrapKind kind, jit_value_t pointer, const char* detail = nullptr);
	//! Result storage types implied by block type `bt`.
	//! \param bt the block type to inspect
	//! \return the storage types the construct produces on exit
	std::vector<WASM::StorageType> storageTypesForBlockType(const WASM::BlockType& bt) const;

	//! The `VMContext*` argument of the function being compiled.
	//! \return the `VMContext*` parameter of the function being compiled, as a
	//! LibJIT value
	jit_value_t vmContextValue();
	//! Bounds-check `index` against `table` and return it (trap with `kind` on failure).
	//! \param table the table pointer the index is checked against
	//! \param index the candidate index, the table’s declared index type
	//! \param opname the instruction name used in the trap message
	//! \param kind the trap kind reported when the index is out of range
	//! \return the index as a 32-bit value, once it is known to be in range
	jit_value_t checkedTableIndex(jit_value_t table, jit_value_t index, const char* opname,
			WASM::TrapKind kind = WASM::TrapKind::UndefinedElement);
	//! Emits code yielding the LinearMemory* for a (constant) memory index.
	//! \param memidx the memory index
	//! \return a pointer value to that memory’s `LinearMemory`
	jit_value_t memoryPointerForIndex(WASM::MemIdx memidx);
	//! The declared index type (i32/i64) of memory `memidx`, for the address
	//! widening in effectiveMemoryAddress.
	//! \param memidx the memory index
	//! \return the declared index type of that memory
	WASM::MemoryIndexType memoryIndexTypeForIndex(WASM::MemIdx memidx) const;
	//! True when table `tableidx` declares a 64-bit index (flag bit 2).
	//! \param tableidx the table index
	//! \return true when the table declares a 64-bit index type
	bool tableIs64Bit(WASM::TableIdx tableidx) const;

	//! Emits code yielding the TableInstance* for a (constant) table index.
	//! \param tableidx the table index
	//! \return a pointer value to that table’s `TableInstance`
	jit_value_t tablePointerForIndex(WASM::TableIdx tableidx);
	//! Emits a runtime check that `callablePtr`'s canonical type id matches the
	//! module-local type index `typeIdx` (id fast path, registry fallback).
	//! \param callablePtr the jit_value_t operand
	//! \param typeIdx the type index
	void emitCallableTypeCheck(jit_value_t callablePtr, WASM::TypeIdx typeIdx);
	//! A lowering the backend cannot perform (in practice: an unmodelled value
	//! type, docs/UNMODELLED_VALUE_TYPES.md). `notImplemented()` throws, which
	//! surfaces as a clean instantiation failure; the LibJIT build region in
	//! ModuleCompiler::compileFunctions is RAII-guarded (docs/SAFETY.md).
	//! \param name the lowering that cannot be performed, used in the exception
	//! message
	[[noreturn]] void notImplemented(const char* name);

	//! Effective address for `ma` with access size `accessSize` (bounds-checked).
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param accessSize the number of bytes the access touches
	//! \return the effective address, widened to the host pointer width
	jit_value_t effectiveMemoryAddress(const WASM::MemArg& ma, jit_nint accessSize);

	//! \par 0xFE atomics (docs/proposals/THREADS.md TH-1)
	//! The shared helpers in WasmBase (wasm_atomic.hpp) perform the access with
	//! `__atomic_*` at seq_cst and trap on a misaligned effective address; these
	//! emit the call and adapt the value types. `effectiveMemoryAddress` has
	//! already bounds-checked the address (and trapped on out-of-bounds).
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param resultType the LibJIT type the loaded value is widened to
	//! \return the loaded value
	jit_value_t emitAtomicLoad(const WASM::MemArg& ma, uint32_t width, jit_type_t resultType);
	//! Atomic store of the top stack value through `ma` (`width` bytes).
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the 32-bit immediate of the instruction
	void        emitAtomicStore(const WASM::MemArg& ma, uint32_t width);
	//! Atomic RMW of the top stack value through `ma`; pushes the previous value.
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param op the RMW operation selector passed to the shared helper
	//! \param resultType the LibJIT type of the previous value the helper returns
	//! \return the value the slot held before the operation
	jit_value_t emitAtomicRmw(const WASM::MemArg& ma, uint32_t width, uint32_t op,
							  jit_type_t resultType);
	//! Atomic compare-exchange through `ma`; pops expected/replacement, pushes old.
	//! \param ma the memarg immediate: the alignment hint and the static byte offset
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param resultType the LibJIT type of the previous value the helper returns
	//! \return the value the slot held before the compare-exchange
	jit_value_t emitAtomicCmpxchg(const WASM::MemArg& ma, uint32_t width,
							  jit_type_t resultType);
	//! Atomic load from a precomputed `address` (shared-memory / GC paths).
	//! \param address the already bounds-checked effective address
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param ordering the memory ordering selector of the instruction
	//! \param resultType the LibJIT type the loaded value is widened to
	//! \return the loaded value
	jit_value_t emitAtomicLoadAddress(jit_value_t address, uint32_t width,
									  uint8_t ordering, jit_type_t resultType);
	//! Atomic store of `value` to a precomputed `address`.
	//! \param address the already bounds-checked effective address
	//! \param value the value that is stored
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param ordering the memory ordering selector of the instruction
	void        emitAtomicStoreRaw(jit_value_t address, jit_value_t value,
								   uint32_t width, uint8_t ordering);
	//! Atomic RMW of `value` at a precomputed `address`; returns the previous value.
	//! \param address the already bounds-checked effective address
	//! \param value the operand of the read-modify-write
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param op the RMW operation selector passed to the shared helper
	//! \param ordering the memory ordering selector of the instruction
	//! \param resultType the LibJIT type of the previous value the helper returns
	//! \return the value the slot held before the operation
	jit_value_t emitAtomicRmwRaw(jit_value_t address, jit_value_t value,
								 uint32_t width, uint32_t op, uint8_t ordering,
								 jit_type_t resultType);
	//! Atomic compare-exchange at a precomputed `address`.
	//! \param address the already bounds-checked effective address
	//! \param expected the value the slot is compared against
	//! \param replacement the value written when the comparison matches
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param ordering the memory ordering selector of the instruction
	//! \param resultType the LibJIT type of the previous value the helper returns
	//! \return the value the slot held before the compare-exchange
	jit_value_t emitAtomicCmpxchgRaw(jit_value_t address, jit_value_t expected,
									 jit_value_t replacement, uint32_t width,
									 uint8_t ordering, jit_type_t resultType);
	//! Atomic store of the top stack value to a precomputed `address`.
	//! \param address the already bounds-checked effective address
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param ordering the memory ordering selector of the instruction
	void        emitAtomicStoreAddress(jit_value_t address, uint32_t width,
									   uint8_t ordering);
	//! Atomic RMW of the top stack value at a precomputed `address`.
	//! \param address the already bounds-checked effective address
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param op the RMW operation selector passed to the shared helper
	//! \param ordering the memory ordering selector of the instruction
	//! \param resultType the LibJIT type of the previous value the helper returns
	//! \return the value the slot held before the operation
	jit_value_t emitAtomicRmwAddress(jit_value_t address, uint32_t width,
									 uint32_t op, uint8_t ordering,
									 jit_type_t resultType);
	//! Atomic compare-exchange at a precomputed `address` (pops expected/replacement).
	//! \param address the already bounds-checked effective address
	//! \param width the access width in bytes (1, 2, 4 or 8)
	//! \param ordering the memory ordering selector of the instruction
	//! \param resultType the LibJIT type of the previous value the helper returns
	//! \return the value the slot held before the compare-exchange
	jit_value_t emitAtomicCmpxchgAddress(jit_value_t address, uint32_t width,
										 uint8_t ordering, jit_type_t resultType);
	//! Address of the atomic slot for an element of table `tableIdx`.
	//! \param tableIdx the table index
	//! \return a pointer value to the slot the atomic access targets
	jit_value_t tableAtomicSlotAddress(WASM::TableIdx tableIdx);
	//! Address of atomic struct field `fieldIdx` of type `typeIdx`.
	//! \param typeIdx the module-local type index of the struct type
	//! \param fieldIdx the position of the field in the struct
	//! \return a pointer value to that field’s storage
	jit_value_t structAtomicFieldAddress(WASM::TypeIdx typeIdx, uint32_t fieldIdx);
	//! Address of an atomic array element of type `typeIdx` (index on the stack).
	//! \param typeIdx the module-local type index of the array type
	//! \return a pointer value to the addressed element’s storage
	jit_value_t arrayAtomicElementAddress(WASM::TypeIdx typeIdx);
	//! Call native helper `fptr` with the given signature and arguments.
	//! \param fptr the address of the native helper to call
	//! \param resultType the LibJIT return type of the helper
	//! \param paramTypes the parameter types of the helper
	//! \param args the argument values to pass, in declaration order
	//! \return the helper’s result value
	jit_value_t callNativeHelper(void* fptr, jit_type_t resultType,
								 std::vector<jit_type_t> paramTypes,
								 std::vector<jit_value_t> args);
	//! Unsigned 32-bit constant used as an atomic ordering / RMW opcode argument.
	//! \param value the constant that is materialised
	//! \return a `uint32_t` constant value carrying `value`
	jit_value_t atomicUintConstant(uint32_t value);

	//! \par Exceptions (docs/proposals/EXCEPTIONS.md EX-3/EX-4)
	//! Handlers are resolved statically: `throw` knows its tag at compile time,
	//! `throw_ref` emits a comparison chain. With no handler in this function
	//! the exception becomes *pending* and the function returns zeroes; every
	//! call site checks the pending slot (EXD1).
	//! \param kind the construct that opens the frame
	//! \param arg the block type that declares its parameters and results
	//! \param catches the decoded catch clauses of a `try_table`, empty otherwise
	void pushBlockFrame(ControlBlock::Kind kind, const WASM::BlockType& arg,
						std::vector<WASM::CatchClause>&& catches);
	//! Backend helper: `tagPayloadType`.
	//! \param tagIdx the tag index, imports first
	//! \return the signature of the exception payload that tag carries
	WASM::FuncType tagPayloadType(WASM::TagIdx tagIdx) const;
	//! Backend helper: `tagIdForClause`.
	//! \param clause the decoded catch clause
	//! \return the canonical identity of the tag the clause catches
	WASM::TagId tagIdForClause(const WASM::CatchClause& clause) const;
	//! Backend helper: `createException`.
	//! \param tagId the canonical identity of the thrown tag
	//! \param payload the signature of the payload that is attached
	//! \return a pointer value to the freshly allocated exception object
	jit_value_t createException(WASM::TagId tagId, const WASM::FuncType& payload);
	//! Backend helper: `storeExceptionPayload`.
	//! \param exnPayload the exception object the payload is stored in
	//! \param payload the signature of that payload
	//! \param values the payload values, in declaration order
	void storeExceptionPayload(jit_value_t exnPayload, const WASM::FuncType& payload,
							   const std::vector<jit_value_t>& values);
	//! Backend helper: `loadExceptionPayload`.
	//! \param exnPayload the exception object the payload is read from
	//! \param payload the signature of that payload
	//! \return the payload values, in declaration order
	std::vector<jit_value_t> loadExceptionPayload(jit_value_t exnPayload,
												  const WASM::FuncType& payload);
	//! Backend helper: `emitThrowToTarget`.
	//! \param exn the exception object that is thrown
	//! \param values the payload values carried by the throw
	//! \param target the control frame whose handler catches it
	//! \param carryExn true when the exception object must also be left on the
	//! model stack for a `catch_ref` handler
	void emitThrowToTarget(jit_value_t exn, std::vector<jit_value_t> values,
						   ControlBlock& target, bool carryExn);
	//! Emits the static dispatch chain for `exn` over the enclosing try_tables;
	//! when no clause matches, sets the pending slot and returns zeroes. All
	//! emitted paths leave the current block.
	//! \param exn the jit_value_t operand
	void emitExceptionPropagation(jit_value_t exn);
	//! Backend helper: `emitPendingException`.
	//! \return the zero value the leaking frame returns; the exception itself is
	//! left in the pending slot
	jit_value_t emitPendingException();
	//! Used after every call: dispatches a pending exception locally or
	//! propagates it, leaving the continuation reachable when none is pending.
	void emitPendingExceptionCheck();
	//! Returns zeroes of the current function's result types (the ABI still
	//! requires a well-formed return when an exception propagates).
	void emitReturnZeroes();

	//! \par Trap status (docs/TRAP_CLEANUP.md §8.5)
	//! A trap is *reported*, not thrown or jumped: generated code calls
	//! `wasm_trap_report` and branches to this function's error epilogue, which
	//! returns zeroes. Every call that can report is followed by a pending check.
	//! No unwind table and no `longjmp` is involved, so a helper may hold a
	//! `std::string` across the trap point.
	void emitPendingTrapCheck();
	//! Backend helper: `emitReportTrap`.
	//! \param kind the trap kind that is reported to the pending slot
	//! \param detail the free-text detail stored with it, or null for the kind’s
	//! spec wording
	void emitReportTrap(WASM::TrapKind kind, const char* detail);
	//! Backend helper: `requestTrapEpilogue`.
	//! Marks that this function needs the shared error epilogue and records the
	//! current trap site for it.
	void requestTrapEpilogue();
	//! Backend helper: `jit_label_undefined;`.
	//! Label of the shared error epilogue, bound by emitTrapEpilogue() when any
	//! trap path was emitted.
	jit_label_t trapEpilogueLabel = jit_label_undefined;
	//! Backend helper: `false;`.
	//! True once a trap path branched to the error epilogue.
	bool trapEpilogueUsed = false;
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

	//! Backend helper: `globalTypeForIndex`.
	//! \param idx the global index, imports first
	//! \return the declared type of that global
	WASM::GlobalType globalTypeForIndex(WASM::GlobalIdx idx) const;

	//! Deterministic-profile float canonicalisation (PROFILES.md PR-3): returns
	//! `value` unchanged unless `deterministicFloats` is set and the value is a
	//! NaN, in which case a canonical NaN of the same width is returned.
	//! \param value the floating-point value produced by the instruction
	//! \param isDouble true for an f64 value, false for an f32 value
	//! \return `value` itself unless canonicalisation is active and it is a NaN,
	//! in which case a canonical NaN of the same width
	jit_value_t canonicalizeFloat(jit_value_t value, bool isDouble);

public:
	//! Create the lowering dispatcher for one function body.
	//! Each argument is borrowed for the lifetime of the dispatcher: the
	//! compilation context and function it emits into, the translator and
	//! instance the body may reference, and the model stacks the caller owns.
	//! \param context the LibJIT compilation context the body is built in
	//! \param function the LibJIT function being filled with instructions
	//! \param typeTranslator the Wasm to LibJIT type mapping of this module
	//! \param instance the instance whose entities the body may touch
	//! \param internals the backend-private tables that belong to `instance`
	//! \param module the parsed module the body comes from; not owned
	//! \param currentFunc the signature of the body being compiled
	//! \param importedFuncCount the number of imported functions that precede
	//! the local bodies, so that a global function index can be rebased
	//! \param functionIndex the global index of the body being compiled
	//! \param branchHints the branch hints of this body, already rebased onto
	//! the opcode stream; empty when the module has none
	//! \param locals the caller-owned local slots of the body
	//! \param valueStack the caller-owned model operand stack
	//! \param controlStack the caller-owned stack of open control frames
	//! \param profileConfig the active profile, which selects the
	//! deterministic-float lowering
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
	void emitImplicitFunctionReturn();

	//! Emits the function prologue: the guest call-stack guard and the pending
	//! check that turns a guard trap into an early return. Call before readCode().
	void emitFunctionEntry();

	//! Emits the shared error epilogue (return zeroes) when any trap path was
	//! emitted. Call after emitImplicitFunctionReturn().
	void emitTrapEpilogue();

	//! OpcodeDispatcher interface
protected:
	//! `internals.translatedTypes` is parallel to `module.typeIds` (filled in translateTypes).
	//! \param idx the module-local type index
	//! \return the LibJIT type `internals.translatedTypes[idx]` holds
	jit_type_t jitTypeForTypeIdx(WASM::TypeIdx idx) const;
	//! Backend helper: `jitTypeForValueType`.
	//! \param vt the Wasm value type to translate
	//! \return the LibJIT type that carries that value type
	jit_type_t jitTypeForValueType(const WASM::ValueType& vt);
	//! Backend helper: `continuationKindFor`.
	//! \param vt the value type of a continuation’s payload slot
	//! \return the one-byte kind tag stored alongside the value in a continuation
	//! buffer
	static std::uint8_t continuationKindFor(const WASM::ValueType& vt);
	//! Backend helper: `allocateContinuationBuffer`.
	//! \param count the number of values the continuation buffer must hold
	//! \return a pointer value to a freshly allocated buffer of that many slots
	jit_value_t allocateContinuationBuffer(std::size_t count);
	//! Backend helper: `storeContinuationValues`.
	//! \param values the values to store, in declaration order
	//! \param types the storage type of each value, in the same order
	//! \param buffer the continuation buffer they are written into
	void storeContinuationValues(const std::vector<jit_value_t>& values,
			const std::vector<WASM::StorageType>& types, jit_value_t buffer);
	//! Backend helper: `loadContinuationValues`.
	//! \param buffer the continuation buffer the values are read from
	//! \param types the storage type of each value, in declaration order
	void loadContinuationValues(jit_value_t buffer,
			const std::vector<WASM::StorageType>& types);
	//! Backend helper: `dropContinuationValues`.
	//! \param buffer the continuation buffer whose references are released
	//! \param types the storage type of each value, in declaration order
	void dropContinuationValues(jit_value_t buffer,
			const std::vector<WASM::StorageType>& types);

	//! Lowering hook: trap unconditionally.
	//! Lower this opcode into the backend IR.
	void dispatchUnreachable() override;
	//! Lowering hook: no operation.
	//! Lower this opcode into the backend IR.
	void dispatchNop() override;
	//! Lowering hook: begin a block construct.
	//! Lower this opcode into the backend IR.
	//! \param arg the block type: the parameter and result types of the construct
	void dispatchBlock(const WASM::BlockType& arg) override;
	//! Lowering hook: begin a loop construct.
	//! Lower this opcode into the backend IR.
	//! \param arg the block type: the parameter and result types of the construct
	void dispatchLoop(const WASM::BlockType& arg) override;
	//! Lowering hook: begin a conditional construct.
	//! Lower this opcode into the backend IR.
	//! \param arg the block type: the parameter and result types of the construct
	void dispatchIf(const WASM::BlockType& arg) override;
	//! Lowering hook: begin the else arm.
	//! Lower this opcode into the backend IR.
	void dispatchElse() override;
	//! Lowering hook: throw an exception.
	//! Lower this opcode into the backend IR.
	//! \param arg the tag index of the exception
	void dispatchThrow(WASM::TagIdx arg) override;
	//! Lowering hook: rethrow an exception taken from the stack.
	//! Lower this opcode into the backend IR.
	void dispatchThrowRef() override;
	//! Lowering hook: create a continuation.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchContNew(WASM::TypeIdx arg) override;
	//! Lowering hook: bind arguments to a continuation.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index of the continuation being bound
	//! \param arg2 the type index of the continuation that results
	void dispatchContBind(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override;
	//! Lowering hook: suspend the current continuation.
	//! Lower this opcode into the backend IR.
	//! \param arg the tag index of the exception
	void dispatchSuspend(WASM::TagIdx arg) override;
	//! Lowering hook: resume a continuation.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	//! \param handlers the decoded `(on ...)` handler clauses of the instruction
	void dispatchResume(WASM::TypeIdx arg, std::vector<WASM::ContHandlerClause>&& handlers) override;
	//! Dispatch the `ResumeThrow` instruction. Lower this opcode into the backend IR.
	//! \param arg1 the type index of the continuation being resumed
	//! \param arg2 the tag index of the exception thrown into it
	//! \param handlers the decoded `(on ...)` handler clauses of the instruction
	void dispatchResumeThrow(WASM::TypeIdx arg1, WASM::TagIdx arg2,
			std::vector<WASM::ContHandlerClause>&& handlers) override;
	//! Dispatch the `ResumeThrowRef` instruction. Lower this opcode into the backend IR.
	//! \param arg the type index of the continuation being resumed
	//! \param handlers the decoded `(on ...)` handler clauses of the instruction
	void dispatchResumeThrowRef(WASM::TypeIdx arg,
			std::vector<WASM::ContHandlerClause>&& handlers) override;
	//! Lowering hook: switch to another continuation.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index of the continuation to switch to
	//! \param arg2 the tag index of the exception that is switched on
	void dispatchSwitch(WASM::TypeIdx arg1, WASM::TagIdx arg2) override;
	//! Resolve a resumed continuation's results: the values the callee left in
	//! `resultBuffer` are pushed onto the model stack, and a tag that the
	//! continuation suspended with is turned back into a throw.
	//! \param func the signature of the resumed continuation
	//! \param handlers the decoded `(on ...)` handler clauses of the `resume`
	//! \param resultAddr the address of the result buffer the callee filled
	//! \param resultBuffer the result tuple the values are copied from
	//! \param tagBuffer the address that carries the suspending tag, or null
	//! \param baseDepth the model stack depth the continuation was entered at
	void emitContinuationResultHandling(const WASM::FuncType& func,
			const std::vector<WASM::ContHandlerClause>& handlers,
			jit_value_t resultAddr, jit_value_t resultBuffer,
			jit_value_t tagBuffer, size_t baseDepth);
	//! Lowering hook: end the current block construct.
	//! Lower this opcode into the backend IR.
	void dispatchEnd() override;
	//! Lowering hook: unconditional branch to a label.
	//! Lower this opcode into the backend IR.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBr(WASM::LabelIdx arg) override;
	//! Lowering hook: branch to a label when the condition is non-zero.
	//! Lower this opcode into the backend IR.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBrIf(WASM::LabelIdx arg) override;
	//! Lowering hook: branch through a table of labels.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the jump table: one label index per case
	//! \param arg2 the default label index taken when no case matches
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) override;
	//! Lowering hook: return from the current function.
	//! Lower this opcode into the backend IR.
	void dispatchReturn() override;
	//! Lowering hook: direct call of a function by index.
	//! Lower this opcode into the backend IR.
	//! \param arg the function index
	void dispatchCall(WASM::FuncIdx arg) override;
	//! Lowering hook: indirect call through a table element.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index of the callee signature
	//! \param arg2 the table the callee is fetched from
	void dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override;
	//! Lowering hook: tail call of a function by index.
	//! Lower this opcode into the backend IR.
	//! \param arg the function index
	void dispatchReturnCall(WASM::FuncIdx arg) override;
	//! Lowering hook: tail call through a table element.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index of the callee signature
	//! \param arg2 the table the callee is fetched from
	void dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override;
	//! Lowering hook: call through a function reference.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchCallRef(WASM::TypeIdx arg) override;
	//! Lowering hook: tail call through a function reference.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchReturnCallRef(WASM::TypeIdx arg) override;
	//! Lowering hook: begin a `try_table` construct.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the block type of the construct
	//! \param arg2 the decoded catch clauses of the table
	void dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) override;
	//! Lowering hook: discard the top of the operand stack.
	//! Lower this opcode into the backend IR.
	void dispatchDrop() override;
	//! Lowering hook: select one of two operands by a condition.
	//! Lower this opcode into the backend IR.
	void dispatchSelect() override;
	//! Lowering hook: select with an explicit list of result types.
	//! Lower this opcode into the backend IR.
	//! \param arg the explicit result types of the `select`
	void dispatchSelectT(std::vector<WASM::ValueType>&& arg) override;
	//! Lowering hook: push the value of a local.
	//! Lower this opcode into the backend IR.
	//! \param arg the local index
	void dispatchLocalGet(WASM::LocalIdx arg) override;
	//! Lowering hook: pop a value into a local.
	//! Lower this opcode into the backend IR.
	//! \param arg the local index
	void dispatchLocalSet(WASM::LocalIdx arg) override;
	//! Lowering hook: write a local and keep the value on the stack.
	//! Lower this opcode into the backend IR.
	//! \param arg the local index
	void dispatchLocalTee(WASM::LocalIdx arg) override;
	//! Lowering hook: push the value of a global.
	//! Lower this opcode into the backend IR.
	//! \param arg the global index
	void dispatchGlobalGet(WASM::GlobalIdx arg) override;
	//! Lowering hook: pop a value into a global.
	//! Lower this opcode into the backend IR.
	//! \param arg the global index
	void dispatchGlobalSet(WASM::GlobalIdx arg) override;
	//! Lowering hook: read a table element.
	//! Lower this opcode into the backend IR.
	//! \param arg the table index
	void dispatchTableGet(WASM::TableIdx arg) override;
	//! Lowering hook: write a table element.
	//! Lower this opcode into the backend IR.
	//! \param arg the table index
	void dispatchTableSet(WASM::TableIdx arg) override;
	//! Lowering hook: i32 load from memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load(WASM::MemArg addr) override;
	//! Lowering hook: i64 load from memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load(WASM::MemArg addr) override;
	//! Lowering hook: f32 load from memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF32Load(WASM::MemArg addr) override;
	//! Lowering hook: f64 load from memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF64Load(WASM::MemArg addr) override;
	//! Lowering hook: i32 load of 8 bits, sign-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load8S(WASM::MemArg addr) override;
	//! Lowering hook: i32 load of 8 bits, zero-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load8U(WASM::MemArg addr) override;
	//! Lowering hook: i32 load of 16 bits, sign-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load16S(WASM::MemArg addr) override;
	//! Lowering hook: i32 load of 16 bits, zero-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load16U(WASM::MemArg addr) override;
	//! Lowering hook: i64 load of 8 bits, sign-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load8S(WASM::MemArg addr) override;
	//! Lowering hook: i64 load of 8 bits, zero-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load8U(WASM::MemArg addr) override;
	//! Lowering hook: i64 load of 16 bits, sign-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load16S(WASM::MemArg addr) override;
	//! Lowering hook: i64 load of 16 bits, zero-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load16U(WASM::MemArg addr) override;
	//! Lowering hook: i64 load of 32 bits, sign-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load32S(WASM::MemArg addr) override;
	//! Lowering hook: i64 load of 32 bits, zero-extended.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load32U(WASM::MemArg addr) override;
	//! Lowering hook: i32 store to memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Store(WASM::MemArg addr) override;
	//! Lowering hook: i64 store to memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store(WASM::MemArg addr) override;
	//! Lowering hook: f32 store to memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF32Store(WASM::MemArg addr) override;
	//! Lowering hook: f64 store to memory.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF64Store(WASM::MemArg addr) override;
	//! Lowering hook: i32 store of the low 8 bits.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Store8(WASM::MemArg addr) override;
	//! Lowering hook: i32 store of the low 16 bits.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Store16(WASM::MemArg addr) override;
	//! Lowering hook: i64 store of the low 8 bits.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store8(WASM::MemArg addr) override;
	//! Lowering hook: i64 store of the low 16 bits.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store16(WASM::MemArg addr) override;
	//! Lowering hook: i64 store of the low 32 bits.
	//! Lower this opcode into the backend IR.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store32(WASM::MemArg addr) override;
	//! Lowering hook: current memory size in pages.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchMemorySize(WASM::MemIdx arg) override;
	//! Lowering hook: grow memory by a number of pages.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchMemoryGrow(WASM::MemIdx arg) override;
	//! Lowering hook: i32 literal constant.
	//! Lower this opcode into the backend IR.
	//! \param arg the 32-bit immediate of the instruction
	void dispatchI32Const(int32_t arg) override;
	//! Lowering hook: i64 literal constant.
	//! Lower this opcode into the backend IR.
	//! \param arg the int64_t operand
	void dispatchI64Const(int64_t arg) override;
	//! Lowering hook: f32 literal constant.
	//! Lower this opcode into the backend IR.
	//! \param arg the float operand
	void dispatchF32Const(float arg) override;
	//! Lowering hook: f64 literal constant.
	//! Lower this opcode into the backend IR.
	//! \param arg the double operand
	void dispatchF64Const(double arg) override;
	//! Lowering hook: i32 compare against zero.
	//! Lower this opcode into the backend IR.
	void dispatchI32Eqz() override;
	//! Lowering hook: i32 equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32Eq() override;
	//! Lowering hook: i32 inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32Ne() override;
	//! Lowering hook: i32 signed less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32LtS() override;
	//! Lowering hook: i32 unsigned less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32LtU() override;
	//! Lowering hook: i32 signed greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32GtS() override;
	//! Lowering hook: i32 unsigned greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32GtU() override;
	//! Lowering hook: i32 signed less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32LeS() override;
	//! Lowering hook: i32 unsigned less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32LeU() override;
	//! Lowering hook: i32 signed greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32GeS() override;
	//! Lowering hook: i32 unsigned greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32GeU() override;
	//! Lowering hook: i64 compare against zero.
	//! Lower this opcode into the backend IR.
	void dispatchI64Eqz() override;
	//! Lowering hook: i64 equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64Eq() override;
	//! Lowering hook: i64 inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64Ne() override;
	//! Lowering hook: i64 signed less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64LtS() override;
	//! Lowering hook: i64 unsigned less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64LtU() override;
	//! Lowering hook: i64 signed greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64GtS() override;
	//! Lowering hook: i64 unsigned greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64GtU() override;
	//! Lowering hook: i64 signed less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64LeS() override;
	//! Lowering hook: i64 unsigned less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64LeU() override;
	//! Lowering hook: i64 signed greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64GeS() override;
	//! Lowering hook: i64 unsigned greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64GeU() override;
	//! Lowering hook: f32 equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32Eq() override;
	//! Lowering hook: f32 inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32Ne() override;
	//! Lowering hook: f32 less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32Lt() override;
	//! Lowering hook: f32 greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32Gt() override;
	//! Lowering hook: f32 less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32Le() override;
	//! Lowering hook: f32 greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32Ge() override;
	//! Lowering hook: f64 equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64Eq() override;
	//! Lowering hook: f64 inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64Ne() override;
	//! Lowering hook: f64 less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64Lt() override;
	//! Lowering hook: f64 greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64Gt() override;
	//! Lowering hook: f64 less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64Le() override;
	//! Lowering hook: f64 greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64Ge() override;
	//! Lowering hook: i32 count leading zero bits.
	//! Lower this opcode into the backend IR.
	void dispatchI32Clz() override;
	//! Lowering hook: i32 count trailing zero bits.
	//! Lower this opcode into the backend IR.
	void dispatchI32Ctz() override;
	//! Lowering hook: i32 population count.
	//! Lower this opcode into the backend IR.
	void dispatchI32Popcnt() override;
	//! Lowering hook: i32 wrapping addition.
	//! Lower this opcode into the backend IR.
	void dispatchI32Add() override;
	//! Lowering hook: i32 wrapping subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI32Sub() override;
	//! Lowering hook: i32 wrapping multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchI32Mul() override;
	//! Lowering hook: i32 signed division (traps on zero or overflow).
	//! Lower this opcode into the backend IR.
	void dispatchI32DivS() override;
	//! Lowering hook: i32 unsigned division (traps on zero).
	//! Lower this opcode into the backend IR.
	void dispatchI32DivU() override;
	//! Lowering hook: i32 signed remainder (traps on zero).
	//! Lower this opcode into the backend IR.
	void dispatchI32RemS() override;
	//! Lowering hook: i32 unsigned remainder (traps on zero).
	//! Lower this opcode into the backend IR.
	void dispatchI32RemU() override;
	//! Lowering hook: i32 bitwise and.
	//! Lower this opcode into the backend IR.
	void dispatchI32And() override;
	//! Lowering hook: i32 bitwise or.
	//! Lower this opcode into the backend IR.
	void dispatchI32Or() override;
	//! Lowering hook: i32 bitwise exclusive or.
	//! Lower this opcode into the backend IR.
	void dispatchI32Xor() override;
	//! Lowering hook: i32 shift left.
	//! Lower this opcode into the backend IR.
	void dispatchI32Shl() override;
	//! Lowering hook: i32 arithmetic shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI32ShrS() override;
	//! Lowering hook: i32 logical shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI32ShrU() override;
	//! Lowering hook: i32 rotate left.
	//! Lower this opcode into the backend IR.
	void dispatchI32Rotl() override;
	//! Lowering hook: i32 rotate right.
	//! Lower this opcode into the backend IR.
	void dispatchI32Rotr() override;
	//! Lowering hook: i64 count leading zero bits.
	//! Lower this opcode into the backend IR.
	void dispatchI64Clz() override;
	//! Lowering hook: i64 count trailing zero bits.
	//! Lower this opcode into the backend IR.
	void dispatchI64Ctz() override;
	//! Lowering hook: i64 population count.
	//! Lower this opcode into the backend IR.
	void dispatchI64Popcnt() override;
	//! Lowering hook: i64 wrapping addition.
	//! Lower this opcode into the backend IR.
	void dispatchI64Add() override;
	//! Lowering hook: i64 wrapping subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI64Sub() override;
	//! Lowering hook: i64 wrapping multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchI64Mul() override;
	//! Lowering hook: i64 signed division (traps on zero or overflow).
	//! Lower this opcode into the backend IR.
	void dispatchI64DivS() override;
	//! Lowering hook: i64 unsigned division (traps on zero).
	//! Lower this opcode into the backend IR.
	void dispatchI64DivU() override;
	//! Lowering hook: i64 signed remainder (traps on zero).
	//! Lower this opcode into the backend IR.
	void dispatchI64RemS() override;
	//! Lowering hook: i64 unsigned remainder (traps on zero).
	//! Lower this opcode into the backend IR.
	void dispatchI64RemU() override;
	//! Lowering hook: i64 bitwise and.
	//! Lower this opcode into the backend IR.
	void dispatchI64And() override;
	//! Lowering hook: i64 bitwise or.
	//! Lower this opcode into the backend IR.
	void dispatchI64Or() override;
	//! Lowering hook: i64 bitwise exclusive or.
	//! Lower this opcode into the backend IR.
	void dispatchI64Xor() override;
	//! Lowering hook: i64 shift left.
	//! Lower this opcode into the backend IR.
	void dispatchI64Shl() override;
	//! Lowering hook: i64 arithmetic shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI64ShrS() override;
	//! Lowering hook: i64 logical shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI64ShrU() override;
	//! Lowering hook: i64 rotate left.
	//! Lower this opcode into the backend IR.
	void dispatchI64Rotl() override;
	//! Lowering hook: i64 rotate right.
	//! Lower this opcode into the backend IR.
	void dispatchI64Rotr() override;
	//! Lowering hook: f32 absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchF32Abs() override;
	//! Lowering hook: f32 negation.
	//! Lower this opcode into the backend IR.
	void dispatchF32Neg() override;
	//! Lowering hook: f32 rounding towards positive infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF32Ceil() override;
	//! Lowering hook: f32 rounding towards negative infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF32Floor() override;
	//! Lowering hook: f32 rounding towards zero.
	//! Lower this opcode into the backend IR.
	void dispatchF32Trunc() override;
	//! Lowering hook: f32 round to nearest, ties to even.
	//! Lower this opcode into the backend IR.
	void dispatchF32Nearest() override;
	//! Lowering hook: f32 square root.
	//! Lower this opcode into the backend IR.
	void dispatchF32Sqrt() override;
	//! Lowering hook: f32 addition.
	//! Lower this opcode into the backend IR.
	void dispatchF32Add() override;
	//! Lowering hook: f32 subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchF32Sub() override;
	//! Lowering hook: f32 multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchF32Mul() override;
	//! Lowering hook: f32 division.
	//! Lower this opcode into the backend IR.
	void dispatchF32Div() override;
	//! Lowering hook: f32 minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF32Min() override;
	//! Lowering hook: f32 maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF32Max() override;
	//! Lowering hook: f32 copy sign.
	//! Lower this opcode into the backend IR.
	void dispatchF32Copysign() override;
	//! Lowering hook: f64 absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchF64Abs() override;
	//! Lowering hook: f64 negation.
	//! Lower this opcode into the backend IR.
	void dispatchF64Neg() override;
	//! Lowering hook: f64 rounding towards positive infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF64Ceil() override;
	//! Lowering hook: f64 rounding towards negative infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF64Floor() override;
	//! Lowering hook: f64 rounding towards zero.
	//! Lower this opcode into the backend IR.
	void dispatchF64Trunc() override;
	//! Lowering hook: f64 round to nearest, ties to even.
	//! Lower this opcode into the backend IR.
	void dispatchF64Nearest() override;
	//! Lowering hook: f64 square root.
	//! Lower this opcode into the backend IR.
	void dispatchF64Sqrt() override;
	//! Lowering hook: f64 addition.
	//! Lower this opcode into the backend IR.
	void dispatchF64Add() override;
	//! Lowering hook: f64 subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchF64Sub() override;
	//! Lowering hook: f64 multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchF64Mul() override;
	//! Lowering hook: f64 division.
	//! Lower this opcode into the backend IR.
	void dispatchF64Div() override;
	//! Lowering hook: f64 minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF64Min() override;
	//! Lowering hook: f64 maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF64Max() override;
	//! Lowering hook: f64 copy sign.
	//! Lower this opcode into the backend IR.
	void dispatchF64Copysign() override;
	//! Lowering hook: wrap an i64 into an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32WrapI64() override;
	//! Lowering hook: truncate an f32 to a signed i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncF32S() override;
	//! Lowering hook: truncate an f32 to an unsigned i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncF32U() override;
	//! Lowering hook: truncate an f64 to a signed i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncF64S() override;
	//! Lowering hook: truncate an f64 to an unsigned i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncF64U() override;
	//! Lowering hook: extend a signed i32 to i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64ExtendI32S() override;
	//! Lowering hook: extend an unsigned i32 to i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64ExtendI32U() override;
	//! Lowering hook: truncate an f32 to a signed i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncF32S() override;
	//! Lowering hook: truncate an f32 to an unsigned i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncF32U() override;
	//! Lowering hook: truncate an f64 to a signed i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncF64S() override;
	//! Lowering hook: truncate an f64 to an unsigned i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncF64U() override;
	//! Lowering hook: convert a signed i32 to f32.
	//! Lower this opcode into the backend IR.
	void dispatchF32ConvertI32S() override;
	//! Lowering hook: convert an unsigned i32 to f32.
	//! Lower this opcode into the backend IR.
	void dispatchF32ConvertI32U() override;
	//! Lowering hook: convert a signed i64 to f32.
	//! Lower this opcode into the backend IR.
	void dispatchF32ConvertI64S() override;
	//! Lowering hook: convert an unsigned i64 to f32.
	//! Lower this opcode into the backend IR.
	void dispatchF32ConvertI64U() override;
	//! Lowering hook: demote an f64 to f32.
	//! Lower this opcode into the backend IR.
	void dispatchF32DemoteF64() override;
	//! Lowering hook: convert a signed i32 to f64.
	//! Lower this opcode into the backend IR.
	void dispatchF64ConvertI32S() override;
	//! Lowering hook: convert an unsigned i32 to f64.
	//! Lower this opcode into the backend IR.
	void dispatchF64ConvertI32U() override;
	//! Lowering hook: convert a signed i64 to f64.
	//! Lower this opcode into the backend IR.
	void dispatchF64ConvertI64S() override;
	//! Lowering hook: convert an unsigned i64 to f64.
	//! Lower this opcode into the backend IR.
	void dispatchF64ConvertI64U() override;
	//! Lowering hook: promote an f32 to f64.
	//! Lower this opcode into the backend IR.
	void dispatchF64PromoteF32() override;
	//! Lowering hook: reinterpret f32 bits as an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32ReinterpretF32() override;
	//! Lowering hook: reinterpret f64 bits as an i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64ReinterpretF64() override;
	//! Lowering hook: reinterpret i32 bits as an f32.
	//! Lower this opcode into the backend IR.
	void dispatchF32ReinterpretI32() override;
	//! Lowering hook: reinterpret i64 bits as an f64.
	//! Lower this opcode into the backend IR.
	void dispatchF64ReinterpretI64() override;
	//! Lowering hook: sign-extend the low 8 bits of an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32Extend8S() override;
	//! Lowering hook: sign-extend the low 16 bits of an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32Extend16S() override;
	//! Lowering hook: sign-extend the low 8 bits of an i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64Extend8S() override;
	//! Lowering hook: sign-extend the low 16 bits of an i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64Extend16S() override;
	//! Lowering hook: sign-extend the low 32 bits of an i64.
	//! Lower this opcode into the backend IR.
	void dispatchI64Extend32S() override;
	//! Lowering hook: push a null reference of the given heap type.
	//! Lower this opcode into the backend IR.
	//! \param arg the HeapType operand
	void dispatchRefNull(const WASM::HeapType& arg) override;
	//! Lowering hook: test a reference for null.
	//! Lower this opcode into the backend IR.
	void dispatchRefIsNull() override;
	//! Lowering hook: push a reference to a function.
	//! Lower this opcode into the backend IR.
	//! \param arg the function index
	void dispatchRefFunc(WASM::FuncIdx arg) override;
	//! Lowering hook: compare two references for identity.
	//! Lower this opcode into the backend IR.
	void dispatchRefEq() override;
	//! Lowering hook: trap when the reference is null.
	//! Lower this opcode into the backend IR.
	void dispatchRefAsNonNull() override;
	//! Lowering hook: branch when the reference is null.
	//! Lower this opcode into the backend IR.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBrOnNull(WASM::LabelIdx arg) override;
	//! Lowering hook: branch when the reference is not null.
	//! Lower this opcode into the backend IR.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBrOnNonNull(WASM::LabelIdx arg) override;
	//! Lowering hook: allocate a struct from operands.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStructNew(WASM::TypeIdx arg) override;
	//! Lowering hook: allocate a struct with default field values.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStructNewDefault(WASM::TypeIdx arg) override;
	//! Custom Descriptors (0xFB 32..38).
	//! \param arg the type index
	void dispatchStructNewDesc(WASM::TypeIdx arg) override;
	//! Lowering hook: struct new default with a descriptor.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStructNewDefaultDesc(WASM::TypeIdx arg) override;
	//! Lowering hook: ref get with a descriptor.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchRefGetDesc(WASM::TypeIdx arg) override;
	//! Lowering hook: ref cast with a descriptor equality.
	//! Lower this opcode into the backend IR.
	//! \param arg the HeapType operand
	void dispatchRefCastDescEq(const WASM::HeapType& arg) override;
	//! Lowering hook: ref cast with a descriptor equality null.
	//! Lower this opcode into the backend IR.
	//! \param arg the HeapType operand
	void dispatchRefCastDescEqNull(const WASM::HeapType& arg) override;
	//! Lowering hook: br on cast with a descriptor equality.
	//! Lower this opcode into the backend IR.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	void dispatchBrOnCastDescEq(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	//! Lowering hook: br on cast with a descriptor equality fail.
	//! Lower this opcode into the backend IR.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	void dispatchBrOnCastDescEqFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	//! Lowering hook: read a struct field.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: read a struct field, sign-extended.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: read a struct field, zero-extended.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: write a struct field.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: allocate an array from a length and an element.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchArrayNew(WASM::TypeIdx arg) override;
	//! Lowering hook: allocate an array of default elements.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchArrayNewDefault(WASM::TypeIdx arg) override;
	//! Lowering hook: allocate an array of fixed length from operands.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: allocate an array from a data segment.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: allocate an array from an element segment.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2).
	//! \param dst the type index
	//! \param src the type index
	void dispatchArrayNewArray(WASM::TypeIdx dst, WASM::TypeIdx src) override;
	//! Lowering hook: array new memory.
	//! Lower this opcode into the backend IR.
	//! \param dst the type index
	//! \param src the 32-bit immediate of the instruction
	void dispatchArrayNewMemory(WASM::TypeIdx dst, uint32_t src) override;
	//! Lowering hook: array new table.
	//! Lower this opcode into the backend IR.
	//! \param dst the type index
	//! \param src the 32-bit immediate of the instruction
	void dispatchArrayNewTable(WASM::TypeIdx dst, uint32_t src) override;
	//! Lowering hook: read an array element.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchArrayGet(WASM::TypeIdx arg) override;
	//! Lowering hook: read an array element, sign-extended.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchArrayGetS(WASM::TypeIdx arg) override;
	//! Lowering hook: read an array element, zero-extended.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchArrayGetU(WASM::TypeIdx arg) override;
	//! Lowering hook: write an array element.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchArraySet(WASM::TypeIdx arg) override;
	//! Lowering hook: length of an array.
	//! Lower this opcode into the backend IR.
	void dispatchArrayLen() override;
	//! Lowering hook: fill an array range with a value.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchArrayFill(WASM::TypeIdx arg) override;
	//! Lowering hook: copy a range between two arrays.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the type index
	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override;
	//! Lowering hook: initialise an array range from a data segment.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: initialise an array range from an element segment.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: test a reference against a heap type.
	//! Lower this opcode into the backend IR.
	//! \param arg the HeapType operand
	void dispatchRefTest(const WASM::HeapType& arg) override;
	//! Lowering hook: test a nullable reference against a heap type.
	//! Lower this opcode into the backend IR.
	//! \param arg the HeapType operand
	void dispatchRefTestNull(const WASM::HeapType& arg) override;
	//! Lowering hook: cast a reference to a heap type.
	//! Lower this opcode into the backend IR.
	//! \param arg the HeapType operand
	void dispatchRefCast(const WASM::HeapType& arg) override;
	//! Lowering hook: cast a nullable reference to a heap type.
	//! Lower this opcode into the backend IR.
	//! \param arg the HeapType operand
	void dispatchRefCastNull(const WASM::HeapType& arg) override;
	//! Lowering hook: branch when a cast succeeds.
	//! Lower this opcode into the backend IR.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	void dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	//! Lowering hook: branch when a cast fails.
	//! Lower this opcode into the backend IR.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	void dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	//! Lowering hook: convert an extern reference to an any reference.
	//! Lower this opcode into the backend IR.
	void dispatchAnyConvertExtern() override;
	//! Lowering hook: convert an any reference to an extern reference.
	//! Lower this opcode into the backend IR.
	void dispatchExternConvertAny() override;
	//! Lowering hook: wrap an integer in an i31 reference.
	//! Lower this opcode into the backend IR.
	void dispatchRefI31() override;
	//! Lowering hook: read an i31 reference, sign-extended.
	//! Lower this opcode into the backend IR.
	void dispatchI31GetS() override;
	//! Lowering hook: read an i31 reference, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI31GetU() override;

	//! Stringref core instructions (docs/proposals/STRINGREF.md STR-3).
	//! \param arg the MemIdx operand
	void dispatchStringNewUtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: new WTF-16.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringNewWtf16(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: new lossy UTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringNewLossyUtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: new WTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringNewWtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: const.
	//! Lower this opcode into the backend IR.
	//! \param idx the 32-bit immediate of the instruction
	void dispatchStringConst(uint32_t idx) override;
	//! Lowering hook: string instruction: measure UTF-8.
	//! Lower this opcode into the backend IR.
	void dispatchStringMeasureUtf8() override;
	//! Lowering hook: string instruction: measure WTF-8.
	//! Lower this opcode into the backend IR.
	void dispatchStringMeasureWtf8() override;
	//! Lowering hook: string instruction: measure WTF-16.
	//! Lower this opcode into the backend IR.
	void dispatchStringMeasureWtf16() override;
	//! Lowering hook: string instruction: encode UTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeUtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: encode lossy UTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: encode WTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeWtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: encode WTF-16.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeWtf16(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: concat.
	//! Lower this opcode into the backend IR.
	void dispatchStringConcat() override;
	//! Lowering hook: string instruction: equality.
	//! Lower this opcode into the backend IR.
	void dispatchStringEq() override;
	//! Lowering hook: true when the string is a sequence of Unicode scalar values.
	//! Lower this opcode into the backend IR.
	void dispatchStringIsUsvSequence() override;

	//! stringview facility (docs/proposals/STRINGREF.md STR-4)
	void dispatchStringAsWtf8() override;
	//! Lowering hook: string instruction: view WTF-8 advance.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewWtf8Advance() override;
	//! Lowering hook: string instruction: view WTF-8 encode UTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: view WTF-8 slice.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewWtf8Slice() override;
	//! Lowering hook: string instruction: view WTF-8 encode lossy UTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: view WTF-8 encode WTF-8.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: as WTF-16.
	//! Lower this opcode into the backend IR.
	void dispatchStringAsWtf16() override;
	//! Lowering hook: string instruction: view WTF-16 length.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewWtf16Length() override;
	//! Lowering hook: string instruction: view WTF-16 get code unit.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewWtf16GetCodeUnit() override;
	//! Lowering hook: string instruction: view WTF-16 encode.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf16Encode(WASM::MemIdx arg) override;
	//! Lowering hook: string instruction: view WTF-16 slice.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewWtf16Slice() override;
	//! Lowering hook: string instruction: as iter.
	//! Lower this opcode into the backend IR.
	void dispatchStringAsIter() override;
	//! Lowering hook: string instruction: view iter next.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewIterNext() override;
	//! Lowering hook: string instruction: view iter advance.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewIterAdvance() override;
	//! Lowering hook: string instruction: view iter rewind.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewIterRewind() override;
	//! Lowering hook: string instruction: view iter slice.
	//! Lower this opcode into the backend IR.
	void dispatchStringViewIterSlice() override;

	//! GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
	//! \param arg the type index
	void dispatchStringNewUtf8Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: new WTF-16 array.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStringNewWtf16Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: encode UTF-8 array.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: encode WTF-16 array.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: new lossy UTF-8 array.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: new WTF-8 array.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStringNewWtf8Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: encode lossy UTF-8 array.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: encode WTF-8 array.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx arg) override;
	//! Lowering hook: string instruction: new array impl.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	//! \param helper the void* operand
	//! \param mode the int operand
	void dispatchStringNewArrayImpl(WASM::TypeIdx arg, void* helper, int mode);
	//! Lowering hook: string instruction: encode array impl.
	//! Lower this opcode into the backend IR.
	//! \param arg the type index
	//! \param helper the void* operand
	//! \param mode the int operand
	void dispatchStringEncodeArrayImpl(WASM::TypeIdx arg, void* helper, int mode);
	//! Lowering hook: i32 trunc sat f 32 signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncSatF32S() override;
	//! Lowering hook: i32 trunc sat f 32 unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncSatF32U() override;
	//! Lowering hook: i32 trunc sat f 64 signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncSatF64S() override;
	//! Lowering hook: i32 trunc sat f 64 unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32TruncSatF64U() override;
	//! Lowering hook: i64 trunc sat f 32 signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncSatF32S() override;
	//! Lowering hook: i64 trunc sat f 32 unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncSatF32U() override;
	//! Lowering hook: i64 trunc sat f 64 signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncSatF64S() override;
	//! Lowering hook: i64 trunc sat f 64 unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI64TruncSatF64U() override;
	//! Lowering hook: memory init.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the 32-bit immediate of the instruction
	//! \param arg2 the MemIdx operand
	void dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) override;
	//! Lowering hook: data drop.
	//! Lower this opcode into the backend IR.
	//! \param arg the 32-bit immediate of the instruction
	void dispatchDataDrop(uint32_t arg) override;
	//! Lowering hook: memory copy.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the MemIdx operand
	//! \param arg2 the MemIdx operand
	void dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) override;
	//! Lowering hook: memory fill.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchMemoryFill(WASM::MemIdx arg) override;
	//! Lowering hook: memory discard.
	//! Lower this opcode into the backend IR.
	//! \param arg the MemIdx operand
	void dispatchMemoryDiscard(WASM::MemIdx arg) override;
	//! Lowering hook: rounding variant.
	//! Lower this opcode into the backend IR.
	//! \param sub the 32-bit immediate of the instruction
	void dispatchRoundingVariant(uint32_t sub) override;
	//! Lowering hook: i64 add128.
	//! Lower this opcode into the backend IR.
	void dispatchI64Add128() override;
	//! Lowering hook: i64 sub128.
	//! Lower this opcode into the backend IR.
	void dispatchI64Sub128() override;
	//! Lowering hook: i64 mul wide signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64MulWideS() override;
	//! Lowering hook: i64 mul wide unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI64MulWideU() override;
	//! Lowering hook: i32 itt.
	//! Lower this opcode into the backend IR.
	//! \param immediate the 32-bit immediate of the instruction
	void dispatchI32Itt(uint32_t immediate) override;
	//! Lowering hook: table init.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the 32-bit immediate of the instruction
	//! \param arg2 the table index
	void dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) override;
	//! Lowering hook: elem drop.
	//! Lower this opcode into the backend IR.
	//! \param arg the 32-bit immediate of the instruction
	void dispatchElemDrop(uint32_t arg) override;
	//! Lowering hook: table copy.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the table index
	//! \param arg2 the table index
	void dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) override;
	//! Lowering hook: table grow.
	//! Lower this opcode into the backend IR.
	//! \param arg the table index
	void dispatchTableGrow(WASM::TableIdx arg) override;
	//! Lowering hook: table size.
	//! Lower this opcode into the backend IR.
	//! \param arg the table index
	void dispatchTableSize(WASM::TableIdx arg) override;
	//! Lowering hook: table fill.
	//! Lower this opcode into the backend IR.
	//! \param arg the table index
	void dispatchTableFill(WASM::TableIdx arg) override;
	//! Lowering hook: v128 load from memory.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load(WASM::MemArg arg) override;
	//! Lowering hook: v128 load8x8 signed.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load8x8S(WASM::MemArg arg) override;
	//! Lowering hook: v128 load8x8 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load8x8U(WASM::MemArg arg) override;
	//! Lowering hook: v128 load16x4 signed.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load16x4S(WASM::MemArg arg) override;
	//! Lowering hook: v128 load16x4 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load16x4U(WASM::MemArg arg) override;
	//! Lowering hook: v128 load32x2 signed.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32x2S(WASM::MemArg arg) override;
	//! Lowering hook: v128 load32x2 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32x2U(WASM::MemArg arg) override;
	//! Lowering hook: v128 load8 splat.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load8Splat(WASM::MemArg arg) override;
	//! Lowering hook: v128 load16 splat.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load16Splat(WASM::MemArg arg) override;
	//! Lowering hook: v128 load32 splat.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32Splat(WASM::MemArg arg) override;
	//! Lowering hook: v128 load64 splat.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load64Splat(WASM::MemArg arg) override;
	//! Lowering hook: v128 load32 zero.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32Zero(WASM::MemArg arg) override;
	//! Lowering hook: v128 load64 zero.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load64Zero(WASM::MemArg arg) override;
	//! Lowering hook: v128 store to memory.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Store(WASM::MemArg arg) override;
	//! Lowering hook: v128 load8 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 load16 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 load32 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 load64 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 store8 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 store16 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 store32 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 store64 lane.
	//! Lower this opcode into the backend IR.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Lowering hook: v128 literal constant.
	//! Lower this opcode into the backend IR.
	//! \param arg the std::span<uint8_t> operand
	void dispatchV128Const(std::span<uint8_t> arg) override;
	//! Lowering hook: i8x16 lane-wise shuffle by immediate indices.
	//! Lower this opcode into the backend IR.
	//! \param arg the std::span<uint8_t> operand
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override;
	//! Lowering hook: i8x16 lane-wise dynamic swizzle by an index vector.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Swizzle() override;
	//! Lowering hook: i8x16 splat of a scalar into every lane.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Splat() override;
	//! Lowering hook: i16x8 splat of a scalar into every lane.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Splat() override;
	//! Lowering hook: i32x4 splat of a scalar into every lane.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Splat() override;
	//! Lowering hook: i64x2 splat of a scalar into every lane.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Splat() override;
	//! Lowering hook: f32x4 splat of a scalar into every lane.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Splat() override;
	//! Lowering hook: f64x2 splat of a scalar into every lane.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Splat() override;
	//! Lowering hook: i8x16 extraction of a lane, sign-extended to i32.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI8x16ExtractLaneS(uint8_t arg) override;
	//! Lowering hook: i8x16 extraction of a lane, zero-extended to i32.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI8x16ExtractLaneU(uint8_t arg) override;
	//! Lowering hook: i8x16 replacement of one lane with a scalar.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI8x16ReplaceLane(uint8_t arg) override;
	//! Lowering hook: i16x8 extraction of a lane, sign-extended to i32.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI16x8ExtractLaneS(uint8_t arg) override;
	//! Lowering hook: i16x8 extraction of a lane, zero-extended to i32.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI16x8ExtractLaneU(uint8_t arg) override;
	//! Lowering hook: i16x8 replacement of one lane with a scalar.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI16x8ReplaceLane(uint8_t arg) override;
	//! Lowering hook: i32x4 extraction of a lane.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI32x4ExtractLane(uint8_t arg) override;
	//! Lowering hook: i32x4 replacement of one lane with a scalar.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI32x4ReplaceLane(uint8_t arg) override;
	//! Lowering hook: i64x2 extraction of a lane.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI64x2ExtractLane(uint8_t arg) override;
	//! Lowering hook: i64x2 replacement of one lane with a scalar.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI64x2ReplaceLane(uint8_t arg) override;
	//! Lowering hook: f32x4 extraction of a lane.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF32x4ExtractLane(uint8_t arg) override;
	//! Lowering hook: f32x4 replacement of one lane with a scalar.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF32x4ReplaceLane(uint8_t arg) override;
	//! Lowering hook: f64x2 extraction of a lane.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF64x2ExtractLane(uint8_t arg) override;
	//! Lowering hook: f64x2 replacement of one lane with a scalar.
	//! Lower this opcode into the backend IR.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF64x2ReplaceLane(uint8_t arg) override;
	//! Lowering hook: i8x16 lane-wise equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Eq() override;
	//! Lowering hook: i8x16 lane-wise inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Ne() override;
	//! Lowering hook: i8x16 lane-wise less-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16LtS() override;
	//! Lowering hook: i8x16 lane-wise less-than compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16LtU() override;
	//! Lowering hook: i8x16 lane-wise greater-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16GtS() override;
	//! Lowering hook: i8x16 lane-wise greater-than compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16GtU() override;
	//! Lowering hook: i8x16 lane-wise less-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16LeS() override;
	//! Lowering hook: i8x16 lane-wise less-or-equal compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16LeU() override;
	//! Lowering hook: i8x16 lane-wise greater-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16GeS() override;
	//! Lowering hook: i8x16 lane-wise greater-or-equal compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16GeU() override;
	//! Lowering hook: i16x8 lane-wise equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Eq() override;
	//! Lowering hook: i16x8 lane-wise inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Ne() override;
	//! Lowering hook: i16x8 lane-wise less-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8LtS() override;
	//! Lowering hook: i16x8 lane-wise less-than compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8LtU() override;
	//! Lowering hook: i16x8 lane-wise greater-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8GtS() override;
	//! Lowering hook: i16x8 lane-wise greater-than compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8GtU() override;
	//! Lowering hook: i16x8 lane-wise less-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8LeS() override;
	//! Lowering hook: i16x8 lane-wise less-or-equal compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8LeU() override;
	//! Lowering hook: i16x8 lane-wise greater-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8GeS() override;
	//! Lowering hook: i16x8 lane-wise greater-or-equal compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8GeU() override;
	//! Lowering hook: i32x4 lane-wise equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Eq() override;
	//! Lowering hook: i32x4 lane-wise inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Ne() override;
	//! Lowering hook: i32x4 lane-wise less-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4LtS() override;
	//! Lowering hook: i32x4 lane-wise less-than compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4LtU() override;
	//! Lowering hook: i32x4 lane-wise greater-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4GtS() override;
	//! Lowering hook: i32x4 lane-wise greater-than compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4GtU() override;
	//! Lowering hook: i32x4 lane-wise less-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4LeS() override;
	//! Lowering hook: i32x4 lane-wise less-or-equal compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4LeU() override;
	//! Lowering hook: i32x4 lane-wise greater-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4GeS() override;
	//! Lowering hook: i32x4 lane-wise greater-or-equal compare, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4GeU() override;
	//! Lowering hook: f32x4 lane-wise equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Eq() override;
	//! Lowering hook: f32x4 lane-wise inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Ne() override;
	//! Lowering hook: f32x4 lane-wise less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Lt() override;
	//! Lowering hook: f32x4 lane-wise greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Gt() override;
	//! Lowering hook: f32x4 lane-wise less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Le() override;
	//! Lowering hook: f32x4 lane-wise greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Ge() override;
	//! Lowering hook: f64x2 lane-wise equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Eq() override;
	//! Lowering hook: f64x2 lane-wise inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Ne() override;
	//! Lowering hook: f64x2 lane-wise less-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Lt() override;
	//! Lowering hook: f64x2 lane-wise greater-than compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Gt() override;
	//! Lowering hook: f64x2 lane-wise less-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Le() override;
	//! Lowering hook: f64x2 lane-wise greater-or-equal compare.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Ge() override;
	//! Lowering hook: v128 not.
	//! Lower this opcode into the backend IR.
	void dispatchV128Not() override;
	//! Lowering hook: v128 bitwise and.
	//! Lower this opcode into the backend IR.
	void dispatchV128And() override;
	//! Lowering hook: v128 bitwise and-not.
	//! Lower this opcode into the backend IR.
	void dispatchV128AndNot() override;
	//! Lowering hook: v128 bitwise or.
	//! Lower this opcode into the backend IR.
	void dispatchV128Or() override;
	//! Lowering hook: v128 bitwise exclusive or.
	//! Lower this opcode into the backend IR.
	void dispatchV128Xor() override;
	//! Lowering hook: v128 bitwise select on a per-lane mask.
	//! Lower this opcode into the backend IR.
	void dispatchV128Bitselect() override;
	//! Lowering hook: v128 true when any lane is non-zero.
	//! Lower this opcode into the backend IR.
	void dispatchV128AnyTrue() override;
	//! Lowering hook: i8x16 lane-wise absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Abs() override;
	//! Lowering hook: i8x16 lane-wise negation.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Neg() override;
	//! Lowering hook: i8x16 population count.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Popcnt() override;
	//! Lowering hook: i8x16 true when every lane is non-zero.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16AllTrue() override;
	//! Lowering hook: i8x16 lane sign bits packed into an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Bitmask() override;
	//! Lowering hook: narrow the signed i16x8 lanes into i8x16 with saturation.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16NarrowI16x8S() override;
	//! Lowering hook: narrow the unsigned i16x8 lanes into i8x16 with saturation.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16NarrowI16x8U() override;
	//! Lowering hook: i8x16 lane-wise shift left.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Shl() override;
	//! Lowering hook: i8x16 lane-wise arithmetic shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16ShrS() override;
	//! Lowering hook: i8x16 lane-wise logical shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16ShrU() override;
	//! Lowering hook: i8x16 lane-wise addition.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Add() override;
	//! Lowering hook: i8x16 saturating signed addition.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16AddSatS() override;
	//! Lowering hook: i8x16 saturating unsigned addition.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16AddSatU() override;
	//! Lowering hook: i8x16 lane-wise subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16Sub() override;
	//! Lowering hook: i8x16 saturating signed subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16SubSatS() override;
	//! Lowering hook: i8x16 saturating unsigned subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16SubSatU() override;
	//! Lowering hook: i8x16 signed minimum.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16MinS() override;
	//! Lowering hook: i8x16 unsigned minimum.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16MinU() override;
	//! Lowering hook: i8x16 signed maximum.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16MaxS() override;
	//! Lowering hook: i8x16 unsigned maximum.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16MaxU() override;
	//! Lowering hook: i8x16 unsigned rounded average.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16AvgrU() override;
	//! Lowering hook: pairwise addition of adjacent i8x16 lanes into i16x8, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtAddPairwiseI8x16S() override;
	//! Lowering hook: pairwise addition of adjacent i8x16 lanes into i16x8, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtAddPairwiseI8x16U() override;
	//! Lowering hook: i16x8 lane-wise absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Abs() override;
	//! Lowering hook: i16x8 lane-wise negation.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Neg() override;
	//! Lowering hook: i16x8 Q15 rounding saturating multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Q15MulRSatS() override;
	//! Lowering hook: i16x8 true when every lane is non-zero.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8AllTrue() override;
	//! Lowering hook: i16x8 lane sign bits packed into an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Bitmask() override;
	//! Lowering hook: narrow the signed i32x4 lanes into i16x8 with saturation.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8NarrowI32x4S() override;
	//! Lowering hook: narrow the unsigned i32x4 lanes into i16x8 with saturation.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8NarrowI32x4U() override;
	//! Lowering hook: extend the low i8x16 lanes into i16x8, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtendLowI8x16S() override;
	//! Lowering hook: extend the high i8x16 lanes into i16x8, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtendHighI8x16S() override;
	//! Lowering hook: extend the low i8x16 lanes into i16x8, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtendLowI8x16U() override;
	//! Lowering hook: extend the high i8x16 lanes into i16x8, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtendHighI8x16U() override;
	//! Lowering hook: i16x8 lane-wise shift left.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Shl() override;
	//! Lowering hook: i16x8 lane-wise arithmetic shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ShrS() override;
	//! Lowering hook: i16x8 lane-wise logical shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ShrU() override;
	//! Lowering hook: i16x8 lane-wise addition.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Add() override;
	//! Lowering hook: i16x8 saturating signed addition.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8AddSatS() override;
	//! Lowering hook: i16x8 saturating unsigned addition.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8AddSatU() override;
	//! Lowering hook: i16x8 lane-wise subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Sub() override;
	//! Lowering hook: i16x8 saturating signed subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8SubSatS() override;
	//! Lowering hook: i16x8 saturating unsigned subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8SubSatU() override;
	//! Lowering hook: i16x8 lane-wise multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8Mul() override;
	//! Lowering hook: i16x8 signed minimum.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8MinS() override;
	//! Lowering hook: i16x8 unsigned minimum.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8MinU() override;
	//! Lowering hook: i16x8 signed maximum.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8MaxS() override;
	//! Lowering hook: i16x8 unsigned maximum.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8MaxU() override;
	//! Lowering hook: i16x8 unsigned rounded average.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8AvgrU() override;
	//! Lowering hook: extended multiply of the low i8x16 lanes into i16x8, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtMulLowI8x16S() override;
	//! Lowering hook: extended multiply of the high i8x16 lanes into i16x8, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtMulHighI8x16S() override;
	//! Lowering hook: extended multiply of the low i8x16 lanes into i16x8, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtMulLowI8x16U() override;
	//! Lowering hook: extended multiply of the high i8x16 lanes into i16x8, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8ExtMulHighI8x16U() override;
	//! Lowering hook: pairwise addition of adjacent i16x8 lanes into i32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtAddPairwiseI16x8S() override;
	//! Lowering hook: pairwise addition of adjacent i16x8 lanes into i32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtAddPairwiseI16x8U() override;
	//! Lowering hook: i32x4 lane-wise absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Abs() override;
	//! Lowering hook: i32x4 lane-wise negation.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Neg() override;
	//! Lowering hook: i32x4 true when every lane is non-zero.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4AllTrue() override;
	//! Lowering hook: i32x4 lane sign bits packed into an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Bitmask() override;
	//! Lowering hook: extend the low i16x8 lanes into i32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtendLowI16x8S() override;
	//! Lowering hook: extend the high i16x8 lanes into i32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtendHighI16x8S() override;
	//! Lowering hook: extend the low i16x8 lanes into i32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtendLowI16x8U() override;
	//! Lowering hook: extend the high i16x8 lanes into i32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtendHighI16x8U() override;
	//! Lowering hook: i32x4 lane-wise shift left.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Shl() override;
	//! Lowering hook: i32x4 lane-wise arithmetic shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ShrS() override;
	//! Lowering hook: i32x4 lane-wise logical shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ShrU() override;
	//! Lowering hook: i32x4 lane-wise addition.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Add() override;
	//! Lowering hook: i32x4 lane-wise subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Sub() override;
	//! Lowering hook: i32x4 lane-wise multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4Mul() override;
	//! Lowering hook: i32x4 signed minimum.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4MinS() override;
	//! Lowering hook: i32x4 unsigned minimum.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4MinU() override;
	//! Lowering hook: i32x4 signed maximum.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4MaxS() override;
	//! Lowering hook: i32x4 unsigned maximum.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4MaxU() override;
	//! Lowering hook: dot product of i32x4 against i16x8 lanes, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4DotI16x8S() override;
	//! Lowering hook: extended multiply of the low i16x8 lanes into i32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtMulLowI16x8S() override;
	//! Lowering hook: extended multiply of the high i16x8 lanes into i32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtMulHighI16x8S() override;
	//! Lowering hook: extended multiply of the low i16x8 lanes into i32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtMulLowI16x8U() override;
	//! Lowering hook: extended multiply of the high i16x8 lanes into i32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4ExtMulHighI16x8U() override;
	//! Lowering hook: i64x2 lane-wise absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Abs() override;
	//! Lowering hook: i64x2 lane-wise negation.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Neg() override;
	//! Lowering hook: i64x2 true when every lane is non-zero.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2AllTrue() override;
	//! Lowering hook: i64x2 lane sign bits packed into an i32.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Bitmask() override;
	//! Lowering hook: extend the low i32x4 lanes into i64x2, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtendLowI32x4S() override;
	//! Lowering hook: extend the high i32x4 lanes into i64x2, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtendHighI32x4S() override;
	//! Lowering hook: extend the low i32x4 lanes into i64x2, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtendLowI32x4U() override;
	//! Lowering hook: extend the high i32x4 lanes into i64x2, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtendHighI32x4U() override;
	//! Lowering hook: i64x2 lane-wise shift left.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Shl() override;
	//! Lowering hook: i64x2 lane-wise arithmetic shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ShrS() override;
	//! Lowering hook: i64x2 lane-wise logical shift right.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ShrU() override;
	//! Lowering hook: i64x2 lane-wise addition.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Add() override;
	//! Lowering hook: i64x2 lane-wise subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Sub() override;
	//! Lowering hook: i64x2 lane-wise multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Mul() override;
	//! Lowering hook: i64x2 lane-wise equality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Eq() override;
	//! Lowering hook: i64x2 lane-wise inequality compare.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2Ne() override;
	//! Lowering hook: i64x2 lane-wise less-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2LtS() override;
	//! Lowering hook: i64x2 lane-wise greater-than compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2GtS() override;
	//! Lowering hook: i64x2 lane-wise less-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2LeS() override;
	//! Lowering hook: i64x2 lane-wise greater-or-equal compare, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2GeS() override;
	//! Lowering hook: extended multiply of the low i32x4 lanes into i64x2, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtMulLowI32x4S() override;
	//! Lowering hook: extended multiply of the high i32x4 lanes into i64x2, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtMulHighI32x4S() override;
	//! Lowering hook: extended multiply of the low i32x4 lanes into i64x2, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtMulLowI32x4U() override;
	//! Lowering hook: extended multiply of the high i32x4 lanes into i64x2, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2ExtMulHighI32x4U() override;
	//! Lowering hook: f32x4 rounding towards positive infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Ceil() override;
	//! Lowering hook: f32x4 rounding towards negative infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Floor() override;
	//! Lowering hook: f32x4 rounding towards zero.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Trunc() override;
	//! Lowering hook: f32x4 round to nearest, ties to even.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Nearest() override;
	//! Lowering hook: f32x4 lane-wise absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Abs() override;
	//! Lowering hook: f32x4 lane-wise negation.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Neg() override;
	//! Lowering hook: f32x4 square root.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Sqrt() override;
	//! Lowering hook: f32x4 lane-wise addition.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Add() override;
	//! Lowering hook: f32x4 lane-wise subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Sub() override;
	//! Lowering hook: f32x4 lane-wise multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Mul() override;
	//! Lowering hook: f32x4 division.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Div() override;
	//! Lowering hook: f32x4 minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Min() override;
	//! Lowering hook: f32x4 maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4Max() override;
	//! Lowering hook: f32x4 pseudo-minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4PMin() override;
	//! Lowering hook: f32x4 pseudo-maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4PMax() override;
	//! Lowering hook: f64x2 rounding towards positive infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Ceil() override;
	//! Lowering hook: f64x2 rounding towards negative infinity.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Floor() override;
	//! Lowering hook: f64x2 rounding towards zero.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Trunc() override;
	//! Lowering hook: f64x2 round to nearest, ties to even.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Nearest() override;
	//! Lowering hook: f64x2 lane-wise absolute value.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Abs() override;
	//! Lowering hook: f64x2 lane-wise negation.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Neg() override;
	//! Lowering hook: f64x2 square root.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Sqrt() override;
	//! Lowering hook: f64x2 lane-wise addition.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Add() override;
	//! Lowering hook: f64x2 lane-wise subtraction.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Sub() override;
	//! Lowering hook: f64x2 lane-wise multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Mul() override;
	//! Lowering hook: f64x2 division.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Div() override;
	//! Lowering hook: f64x2 minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Min() override;
	//! Lowering hook: f64x2 maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2Max() override;
	//! Lowering hook: f64x2 pseudo-minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2PMin() override;
	//! Lowering hook: f64x2 pseudo-maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2PMax() override;
	//! Lowering hook: lane-wise saturating truncation of f32x4 into i32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4TruncSatF32x4S() override;
	//! Lowering hook: lane-wise saturating truncation of f32x4 into i32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4TruncSatF32x4U() override;
	//! Lowering hook: lane-wise conversion of i32x4 into f32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4ConvertI32x4S() override;
	//! Lowering hook: lane-wise conversion of i32x4 into f32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4ConvertI32x4U() override;
	//! Lowering hook: lane-wise saturating truncation of f64x2 into the low i32x4 lanes, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4TruncSatF64x2SZero() override;
	//! Lowering hook: lane-wise saturating truncation of f64x2 into the low i32x4 lanes, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4TruncSatF64x2UZero() override;
	//! Lowering hook: conversion of the low i32x4 lanes into f64x2, signed.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2ConvertLowI32x4S() override;
	//! Lowering hook: conversion of the low i32x4 lanes into f64x2, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2ConvertLowI32x4U() override;
	//! Lowering hook: demotion of f64x2 into the low f32x4 lanes.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4DemoteF64x2Zero() override;
	//! Lowering hook: promotion of the low f32x4 lanes into f64x2.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2PromoteLowF32x4() override;

	//! Relaxed SIMD (0x100-0x113): SD5 picks the strict result for each.
	void dispatchI8x16RelaxedSwizzle() override;
	//! Lowering hook: relaxed lane-wise truncation of f32x4 into i32x4, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4RelaxedTruncF32x4S() override;
	//! Lowering hook: relaxed lane-wise truncation of f32x4 into i32x4, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4RelaxedTruncF32x4U() override;
	//! Lowering hook: relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, signed.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4RelaxedTruncF64x2SZero() override;
	//! Lowering hook: relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, unsigned.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4RelaxedTruncF64x2UZero() override;
	//! Lowering hook: f32x4 relaxed fused multiply-add.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4RelaxedMadd() override;
	//! Lowering hook: f32x4 relaxed fused negative multiply-add.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4RelaxedNmadd() override;
	//! Lowering hook: f64x2 relaxed fused multiply-add.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2RelaxedMadd() override;
	//! Lowering hook: f64x2 relaxed fused negative multiply-add.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2RelaxedNmadd() override;
	//! Lowering hook: i8x16 relaxed lane-wise select.
	//! Lower this opcode into the backend IR.
	void dispatchI8x16RelaxedLaneselect() override;
	//! Lowering hook: i16x8 relaxed lane-wise select.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8RelaxedLaneselect() override;
	//! Lowering hook: i32x4 relaxed lane-wise select.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4RelaxedLaneselect() override;
	//! Lowering hook: i64x2 relaxed lane-wise select.
	//! Lower this opcode into the backend IR.
	void dispatchI64x2RelaxedLaneselect() override;
	//! Lowering hook: f32x4 relaxed minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4RelaxedMin() override;
	//! Lowering hook: f32x4 relaxed maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF32x4RelaxedMax() override;
	//! Lowering hook: f64x2 relaxed minimum.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2RelaxedMin() override;
	//! Lowering hook: f64x2 relaxed maximum.
	//! Lower this opcode into the backend IR.
	void dispatchF64x2RelaxedMax() override;
	//! Lowering hook: i16x8 relaxed Q15 rounding saturating multiplication.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8RelaxedQ15MulRS() override;
	//! Lowering hook: relaxed signed dot product of i8x16 and i7x16 lanes into i16x8.
	//! Lower this opcode into the backend IR.
	void dispatchI16x8RelaxedDotI8x16I7x16S() override;
	//! Lowering hook: relaxed signed dot product of i8x16 and i7x16 lanes, accumulated into i32x4.
	//! Lower this opcode into the backend IR.
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override;
	//! Lowering hook: memory atomic notify.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchMemoryAtomicNotify(WASM::MemArg arg) override;
	//! Lowering hook: memory atomic wait32.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchMemoryAtomicWait32(WASM::MemArg arg) override;
	//! Lowering hook: memory atomic wait64.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchMemoryAtomicWait64(WASM::MemArg arg) override;
	//! Lowering hook: atomic fence.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	void dispatchAtomicFence(uint8_t ordering) override;
	//! Lowering hook: atomic pause.
	//! Lower this opcode into the backend IR.
	void dispatchAtomicPause() override;
	//! Lowering hook: global atomic get.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic set.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic read-modify-write addition.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic read-modify-write subtraction.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic read-modify-write and.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic read-modify-write or.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic read-modify-write exclusive or.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic read-modify-write exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: global atomic read-modify-write compare-exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Lowering hook: table atomic get.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override;
	//! Lowering hook: table atomic set.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override;
	//! Lowering hook: table atomic read-modify-write exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override;
	//! Lowering hook: table atomic read-modify-write compare-exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override;
	//! Lowering hook: struct atomic get.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic get signed.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic get unsigned.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic set.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic read-modify-write addition.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic read-modify-write subtraction.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic read-modify-write and.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic read-modify-write or.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic read-modify-write exclusive or.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic read-modify-write exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: struct atomic read-modify-write compare-exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Lowering hook: array atomic get.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic get signed.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic get unsigned.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic set.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic read-modify-write addition.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic read-modify-write subtraction.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic read-modify-write and.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic read-modify-write or.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic read-modify-write exclusive or.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic read-modify-write exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: array atomic read-modify-write compare-exchange.
	//! Lower this opcode into the backend IR.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Lowering hook: i32 atomic load.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicLoad(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic load.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic load8 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic load16 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic load8 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic load16 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic load32 unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic store.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicStore(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic store.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic store8.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicStore8(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic store16.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicStore16(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic store8.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore8(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic store16.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore16(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic store32.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore32(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic read-modify-write add.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic read-modify-write add.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw8 add unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw16 add unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw8 add unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw16 add unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw32 add unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic read-modify-write sub.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic read-modify-write sub.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw8 sub unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw16 sub unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw8 sub unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw16 sub unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw32 sub unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic read-modify-write and.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic read-modify-write and.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw8 and unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw16 and unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw8 and unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw16 and unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw32 and unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic read-modify-write or.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic read-modify-write or.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw8 or unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw16 or unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw8 or unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw16 or unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw32 or unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic read-modify-write xor.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic read-modify-write xor.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw8 xor unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw16 xor unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw8 xor unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw16 xor unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw32 xor unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic read-modify-write exchange.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic read-modify-write exchange.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw8 exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw16 exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw8 exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw16 exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw32 exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic read-modify-write compare-exchange.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic read-modify-write compare-exchange.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw8 compare-exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override;
	//! Lowering hook: i32 atomic rmw16 compare-exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw8 compare-exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw16 compare-exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override;
	//! Lowering hook: i64 atomic rmw32 compare-exchange unsigned.
	//! Lower this opcode into the backend IR.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override;
};

}

#endif // LIBJITOPCODEDISPATCHER_HPP
