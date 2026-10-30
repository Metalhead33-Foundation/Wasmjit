#ifndef WASMVALIDATOR_HPP
#define WASMVALIDATOR_HPP

#include "WasmApi.hpp"

/*!
  \file WasmValidator.hpp
  \brief The validating front end for a decoded core module.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmModule.hpp"
#include "WasmDiagnostics.hpp"
#include "WasmValidation.hpp"
#include "WasmOpcodeDispatcher.hpp"
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace WASM {

//! Structural validator for a decoded module.
//!
//! Scope (first milestone): every rule that does not need the operand type
//! stack — section/index consistency, limits, exports, start, tags, global
//! initialisers, active segment targets, and the *immediate* checks inside
//! function bodies (memory/table/global/local/function/type/tag indices,
//! alignment, lane indices). A body containing an opcode this validator does not
//! model yet is conservatively accepted: the strictness lost there is measured
//! by `WASM_SPEC_VALIDATE=1`, and the engine must never reject a valid module.
//!
//! The dispatcher supplies the immediate decoding, so a new opcode needs no
//! validator-side byte parsing — only its rule. `onUnhandledOpcode` is how the
//! base class reports an opcode the backend/validator does not implement.
class Validator final : public OpcodeDispatcher
{
public:
	//! Validates `module` with `options`; throws `ValidationError` on the first
	//! rule that fails.
	explicit Validator(const Module& module, ValidationOptions options = {});

	//! Throws std::runtime_error naming the first rule that failed.
/**
 * \brief Validates the decoded module and throws on the first structural or instruction-validation failure.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validate();

private:
	struct ValidationError : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	const Module& module; //!< The module under validation.
	ValidationOptions options; //!< The options the validator was started with.
	std::size_t totalFunctions = 0; //!< Declared function count, for the allocation budget.
	std::size_t totalTables = 0; //!< Declared table count, for the allocation budget.
	std::size_t totalMemories = 0; //!< Declared memory count, for the allocation budget.
	std::size_t totalGlobals = 0; //!< Declared global count, for the allocation budget.
	std::size_t totalTags = 0; //!< Declared tag count, for the allocation budget.

	//! Current function body (npos while validating module-level structure).
	static constexpr std::size_t kNoBody = static_cast<std::size_t>(-1);
	//! The body being validated, or `kNoBody` at module level.
	std::size_t currentBody = kNoBody;
	//! The function's local types, parameters first.
	std::vector<ValueType> locals;

	//! Records a structured validation diagnostic and throws `ValidationError`
	//! with the same text. The id is `WASM.VALIDATE.<LEAF>`. An empty `id` asks
	//! `validationRuleId()` to classify the message into a stable rule family;
	//! a site that knows its rule better passes an explicit `DiagnosticId::…`.
	[[noreturn]] void fail(const std::string& message,
			std::string_view id = {}) const;

	//! Diagnostic form of a local index in the body being validated
	//! (`"0 ($name)"`); the name-section outer key is the global function index.
	std::string describeLocal(uint32_t idx) const;

	//! The decoded function type at module-local type index `typeIdx`.
	FuncType funcTypeAt(uint32_t typeIdx) const;
	//! Resolve `cont $ft` to the module-local function type index it names. The
	//! canonical view stores a process-wide TypeId, so the module-local index is
	//! recovered from `typeIds` (STACK_SWITCHING.md SS-1).
	uint32_t continuationFuncIndex(TypeIdx contTypeIdx, const char* who) const;
	//! The decoded function type a `cont $ft` names.
	FuncType continuationFuncType(TypeIdx contTypeIdx, const char* who) const;
	//! The stage-2 provider carries at most one i32 parameter and one i32 result
	//! for a continuation function, a control tag and a handler payload.
	void requireMvpContinuationSignature(const FuncType& type, const char* who) const;
	//! Requires a valid module-local function index.
	void requireFunctionIndex(uint32_t idx) const;
	//! Requires a valid table index.
	void requireTableIndex(uint32_t idx) const;
	//! Requires a valid memory index.
	void requireMemoryIndex(uint32_t idx) const;
	//! Type checks for the `string` array instructions: the type index must be
	//! an array of the required packed element type; encode variants require a
	//! mutable array.
	void requireStringArrayType(TypeIdx idx, ValueTypeCode packed, bool requireMutable) const;
	//! Requires a valid global index.
	void requireGlobalIndex(uint32_t idx) const;
	//! Requires a valid tag index.
	void requireTagIndex(uint32_t idx) const;
	//! Requires a valid local index for the function being validated.
	void requireLocalIndex(uint32_t idx) const;
	//! Requires a valid element-segment index.
	void requireElementIndex(uint32_t idx) const;
	//! Requires a valid data-segment index.
	void requireDataIndex(uint32_t idx) const;
	//! True when global `idx` is declared `mut`.
	bool globalIsMutable(uint32_t idx) const;
	//! True when table `idx`'s element type is a function reference.
	bool tableIsFuncRef(uint32_t idx) const;
	//! True when memory `idx` uses 64-bit addresses.
	bool memoryIs64(uint32_t idx) const;
	//! True when memory `idx` is a shared (threads-proposal) memory.
	bool memoryIsShared(uint32_t idx) const;

	//! Reference subtyping (nullability + the func/extern/exn/any hierarchies).
	bool refSubtype(const ValueType& actual, const ValueType& expected) const;
	//! True when a local of this type has an implicit default value, so reading it
	//! before any store is valid. Non-nullable references do not.
	bool isDefaultable(const ValueType& type) const;
	//! `ref.func x` may only name a function that the module also mentions
	//! outside its function bodies or start function (the spec's `C.refs`).
	std::unordered_set<uint32_t> declaredFunctions;
	//! Validates a constant initialiser, returning its value (or nullopt when
	//! the expression is not constant).
	std::optional<ValueType> validateConstExpr(const std::vector<uint8_t>& bytes,
			const std::string& label, std::size_t globalLimit);
	//! True when `type` is `eqref` or a subtype of it.
	bool isEqRef(const ValueType& type) const {
		return refSubtype(type, ValueType{ValueTypeCode::EqRef, -1});
	}
	//! A typed reference's concrete heap type must name a declared type.
	void requireWellFormedType(const ValueType& type, const std::string& where) const;

	//! `ref.test`/`ref.cast` target construction and the top of the target's
	//! heap hierarchy (used for the operand's same-hierarchy rule).
	ValueType refType(bool nullable, const HeapType& heapType) const;
	//! The top of the heap hierarchy `heapType` belongs to.
	ValueType refTopForHeap(const HeapType& heapType) const;
	//! Validates a `ref.test`'s operands against the target heap type.
	void validateRefTest(bool nullable, const HeapType& heapType);
	//! Validates a `ref.cast`'s operands against the target heap type.
	void validateRefCast(bool nullable, const HeapType& heapType);

	//! Profile gates (docs/proposals/PROFILES.md PR-2/PR-4): reject constructs an
	//! excluded marker forbids before any structural rule reports on them.
/**
 * \brief Checks the Profile constraint for the current module.
 */
	void checkProfile() const;
	//! Rejects a value type an excluded profile marker forbids.
	void checkProfileValueType(const ValueType& type, const std::string& where) const;
	//! Prefixed-instruction gates (shared with the base dispatcher's hooks).
	void onSimdOpcode(SIMDOpcode opcode) override;
	void onAtomicOpcode(AtomicOpcode opcode) override; //!< Validate the stack effect of this opcode.

	//! Checks declared limits against the implementation and the allocation budget.
	void checkLimits(const Limits& limits, bool is64, bool isMemory,
					 const char* what, std::size_t index,
					 uint64_t pageSize = 0x10000) const;
	//! Checks a memory access's alignment and memory index.
	void requireMemoryAccess(const MemArg& arg, uint32_t naturalLog2) const;
	//! Multibyte Array Access (GC_EXTENSIONS.md GX-1): a load/store whose memarg
	//! sets bit 5 operates on a GC array. These helpers validate the type index
	//! and element shape, enforce mutability for stores, and pop `[ref, address]`
	//! instead of the linear-memory address.
	void requireArrayAccess(const MemArg& arg, uint32_t naturalLog2) const;
	//! Requires the array the memarg names to be mutable (for a store).
	void requireArrayMutable(const MemArg& arg) const;
	//! Pops the `[ref, address]` pair an array access takes.
	void popMemoryAddress(const MemArg& arg);
	//! Atomic accesses have extra rules: the alignment immediate must equal the
	//! natural alignment, and the acquire-release ordering byte is 0 or 1.
	void requireAtomicMemoryAccess(const MemArg& arg, uint32_t naturalLog2) const;
	//! The field type at `fieldIndex` of struct type `type`.
	const CanonFieldType& requireStructField(TypeIdx type, uint32_t fieldIndex) const;
	//! Requires `lane` to be a valid index for `laneCount` lanes.
	void requireLane(uint8_t lane, uint32_t laneCount, const char* what) const;

	//! Operand/control typing (core instructions only in this milestone).
	struct ControlFrame {
		//! The operand types the frame requires on entry (block parameters).
		std::vector<ValueType> startTypes;
		//! The operand types the frame must produce on exit.
		std::vector<ValueType> endTypes;
		std::size_t height = 0; //!< Operand-stack height when the frame was pushed.
		bool isLoop = false; //!< True when the frame is a `loop` label.
		bool isIf = false; //!< True when the frame is an `if` label.
		bool elseSeen = false; //!< True once the frame's `else` was seen.
		bool unreachable = false; //!< True while the frame is in unreachable code.
		//! Definite assignment of non-defaultable locals. A frame inherits its
		//! parent's set on entry; stores made inside the frame do not escape it,
		//! and `else` restarts from the entry set (the spec's local-init rule).
		std::vector<uint8_t> initialized;
		//! The `initialized` set as it was on frame entry (restored by `else`).
		std::vector<uint8_t> entryInitialized;
	};
	std::vector<ValueType> typeStack; //!< The polymorphic operand stack.
	std::vector<ControlFrame> controlStack; //!< The active control frames, innermost last.
	//! Set when an opcode without a typing rule is seen: the rest of the body is
	//! still scanned for immediate checks, but the operand stack is not trusted.
	bool typeTainted = false;

/**
 * \brief Resolves frame from the current function and module type context.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ControlFrame& frame();
	//! The parameter types a block type introduces.
	std::vector<ValueType> blockParams(const BlockType& blockType) const;
	//! The result types a block type produces.
	std::vector<ValueType> blockResults(const BlockType& blockType) const;
	//! Pushes one operand type onto the type stack.
	void pushValue(const ValueType& type);
/**
 * \brief Consumes the expected operand-stack value or sequence, validating its type.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	std::optional<ValueType> popAny();
	//! Pops one operand and requires it to be `code`.
	void popExpect(ValueTypeCode code);
	//! Pops one operand and requires it to match `type`.
	void popExpect(const ValueType& type);
	//! Pushes several operand types, in order.
	void pushTypes(const std::vector<ValueType>& types);
	//! Pops several operand types, in reverse order.
	void popTypes(const std::vector<ValueType>& types);
	//! Requires the operand stack to match a tail call's callee results.
	void checkTailCallResults(const FuncType& callee);
	//! True when `actual` is assignable to `expected` (subtyping + defaults).
	bool assignable(const ValueType& actual, const ValueType& expected) const;
	//! True for `cont`, `nocont` and a concrete `cont $ft` type index. The
	//! stack-switching proposal forbids casting to any continuation type.
	bool isContinuationHeap(const HeapType& type) const;
	//! More Array Constructors (GC_EXTENSIONS.md GX-2) helpers.
	CoreTypeView requireArrayType(TypeIdx idx, const char* who) const;
	//! Compares two storage types for assignment compatibility.
	bool storageAssignable(const CanonStorageType& actual,
			const CanonStorageType& expected) const;
	//! Marks the rest of the current block unreachable (the polymorphic stack).
	void setUnreachable();
	//! Opens a block/loop/if frame.
	void beginBlock(const BlockType& blockType, bool isLoop, bool isIf);
	//! Closes a frame (the `else` arm is `isElse`).
	void endBlock(bool isElse);
	//! Type-checks a `br` to `label`.
	void branch(LabelIdx label);
	//! Type-checks a `br_if` to `label`.
	void branchIf(LabelIdx label);
	//! The address value type a memory access uses (memory32 vs memory64).
	ValueTypeCode memoryAddressType(MemIdx memIdx) const;
	//! The value type a table index is expressed in.
	ValueTypeCode tableIndexType(TableIdx tableIdx) const;
	//! The function type at function index `idx`.
	FuncType functionType(FuncIdx idx) const;
	//! The value type of global `idx`.
	ValueType globalType(GlobalIdx idx) const;
	//! The element type of table `idx`.
	ValueType tableElementType(TableIdx idx) const;
	const FuncType* currentFuncType = nullptr; //!< The signature of the body being validated, if any.
	//! The parameter types of `types`, as stack operand types.
	std::vector<ValueType> paramTypes(const FuncType& types) const;
	//! The result types of `types`, as stack operand types.
	std::vector<ValueType> resultTypes(const FuncType& types) const;

	//! Control and parametric instructions.
/**
 * \brief Validates the Unreachable instruction's immediates and operand-stack effect.
 */
	void dispatchUnreachable() override;
	void dispatchNop() override; //!< Validate the stack effect of this opcode.
	void dispatchBlock(const BlockType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchLoop(const BlockType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchIf(const BlockType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchElse() override; //!< Validate the stack effect of this opcode.
	void dispatchEnd() override; //!< Validate the stack effect of this opcode.
	void dispatchBr(LabelIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrIf(LabelIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrTable(std::vector<LabelIdx>&& arg1, LabelIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchReturn() override; //!< Validate the stack effect of this opcode.
	void dispatchDrop() override; //!< Validate the stack effect of this opcode.
	void dispatchSelect() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Const(int32_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Const(int64_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF32Const(float arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF64Const(double arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefIsNull() override; //!< Validate the stack effect of this opcode.
	void dispatchRefEq() override; //!< Validate the stack effect of this opcode.
	void dispatchRefAsNonNull() override; //!< Validate the stack effect of this opcode.

	//! More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2).
	void dispatchArrayNewArray(TypeIdx dst, TypeIdx src) override;
	void dispatchArrayNewMemory(TypeIdx dst, MemIdx src) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayNewTable(TypeIdx dst, TableIdx src) override; //!< Validate the stack effect of this opcode.
	//! Custom Descriptors (0xFB 32..38).
	void dispatchStructNew(TypeIdx arg) override;
	void dispatchStructNewDefault(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStructNewDesc(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStructNewDefaultDesc(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefGetDesc(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefCastDescEq(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefCastDescEqNull(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCastDescEq(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCastDescEqFail(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
	//! Validates a `ref.cast_desc_eq` against its target descriptor type.
	void validateCastDescEq(bool nullableTarget, const HeapType& target);
	//! Validates a `br_on_cast_desc_eq` (and its `_fail` form).
	void validateBrOnCastDescEq(bool isFail, uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2);

/**
 * \brief Validates Module according to the WebAssembly validation rules.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateModule();
	//! Validates one global's initialiser against its declared type.
	void validateGlobalInit(const Global& global, std::size_t index);
	//! Validates one element segment's type, mode and initialiser.
	void validateElementSegment(const ElementSegment& segment, std::size_t index);
	//! Validates one data segment's mode and bounds.
	void validateDataSegment(const DataSegment& segment, std::size_t index);
	//! Validates a table declaration (or import) and its initialiser.
	void validateTableInit(const TableType& table, std::size_t index, bool isImport = false);
	//! Validates one function body's instructions and stack discipline.
	void validateFunctionBody(const FunctionBody& body, std::size_t index);

	void dispatchStringNewUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringNewWtf16(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringNewLossyUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringNewWtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringConst(uint32_t idx) override; //!< Validate the stack effect of this opcode.
	void dispatchStringMeasureUtf8() override; //!< Validate the stack effect of this opcode.
	void dispatchStringMeasureWtf8() override; //!< Validate the stack effect of this opcode.
	void dispatchStringMeasureWtf16() override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeLossyUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeWtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeWtf16(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringConcat() override; //!< Validate the stack effect of this opcode.
	void dispatchStringEq() override; //!< Validate the stack effect of this opcode.
	void dispatchStringIsUsvSequence() override; //!< Validate the stack effect of this opcode.

	//! stringview facility (docs/proposals/STRINGREF.md STR-4)
/**
 * \brief Validates the StringAsWtf8 instruction's immediates and operand-stack effect.
 */
	void dispatchStringAsWtf8() override;
	void dispatchStringViewWtf8Advance() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf8EncodeUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf8Slice() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf8EncodeLossyUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf8EncodeWtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringAsWtf16() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf16Length() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf16GetCodeUnit() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf16Encode(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewWtf16Slice() override; //!< Validate the stack effect of this opcode.
	void dispatchStringAsIter() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewIterNext() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewIterAdvance() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewIterRewind() override; //!< Validate the stack effect of this opcode.
	void dispatchStringViewIterSlice() override; //!< Validate the stack effect of this opcode.

	//! GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
	void dispatchStringNewUtf8Array(TypeIdx arg) override;
	void dispatchStringNewWtf16Array(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeUtf8Array(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeWtf16Array(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringNewLossyUtf8Array(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringNewWtf8Array(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeLossyUtf8Array(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStringEncodeWtf8Array(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void onUnhandledOpcode(const char* name) override; //!< Validate the stack effect of this opcode.

	//! Immediate-only checks (no operand stack yet).
	void dispatchLocalGet(LocalIdx arg) override;
	void dispatchLocalSet(LocalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchLocalTee(LocalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalGet(GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalSet(GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchCall(FuncIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchReturnCall(FuncIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchCallIndirect(TypeIdx arg1, TableIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchReturnCallIndirect(TypeIdx arg1, TableIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchCallRef(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchReturnCallRef(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefFunc(FuncIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefNull(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefTest(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefTestNull(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefCast(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefCastNull(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchThrow(TagIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchThrowRef() override; //!< Validate the stack effect of this opcode.
	void dispatchContNew(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchContBind(TypeIdx arg1, TypeIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchSuspend(TagIdx arg) override; //!< Validate the stack effect of this opcode.
	//! Shared validation for `resume`/`resume_throw*` handler clauses.
	void validateContHandlers(const std::vector<ContHandlerClause>& handlers);
	void dispatchResume(TypeIdx arg, std::vector<ContHandlerClause>&& handlers) override; //!< Validate the stack effect of this opcode.
	//! Dispatch the `ResumeThrow` instruction. Validate the stack effect of this opcode.
	void dispatchResumeThrow(TypeIdx arg1, TagIdx arg2,
			std::vector<ContHandlerClause>&& handlers) override;
	//! Dispatch the `ResumeThrowRef` instruction. Validate the stack effect of this opcode.
	void dispatchResumeThrowRef(TypeIdx arg,
			std::vector<ContHandlerClause>&& handlers) override;
	void dispatchSwitch(TypeIdx arg1, TagIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchTryTable(const BlockType& arg1, std::vector<CatchClause>&& arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchSelectT(std::vector<ValueType>&& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnNull(LabelIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnNonNull(LabelIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCast(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCastFail(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
	//! Validates a `br_on_cast` (or `_fail`) against its two heap types.
	void validateBrOnCast(bool isFail, uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2);

	void dispatchTableGet(TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableSet(TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableGrow(TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableSize(TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableFill(TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableCopy(TableIdx arg1, TableIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchTableInit(uint32_t arg1, TableIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchElemDrop(uint32_t arg) override; //!< Validate the stack effect of this opcode.

	void dispatchMemorySize(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchMemoryGrow(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchMemoryInit(uint32_t arg1, MemIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchDataDrop(uint32_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchMemoryCopy(MemIdx arg1, MemIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchMemoryFill(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchMemoryDiscard(MemIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRoundingVariant(uint32_t sub) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Add128() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Sub128() override; //!< Validate the stack effect of this opcode.
	void dispatchI64MulWideS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64MulWideU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Itt(uint32_t immediate) override; //!< Validate the stack effect of this opcode.

	//! Core memory accesses.
	void dispatchI32Load(MemArg arg) override;
	void dispatchI64Load(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF32Load(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF64Load(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32Load8S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32Load8U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32Load16S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32Load16U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Load8S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Load8U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Load16S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Load16U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Load32S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Load32U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32Store(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Store(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF32Store(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF64Store(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32Store8(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32Store16(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Store8(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Store16(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64Store32(MemArg arg) override; //!< Validate the stack effect of this opcode.


	//! Core numeric instructions (operand/result typing).
/**
 * \brief Validates the I32Eqz instruction's immediates and operand-stack effect.
 */
	void dispatchI32Eqz() override;
	void dispatchI64Eqz() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Ceil() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Floor() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Nearest() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Sqrt() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Trunc() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Ceil() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Floor() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Nearest() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Sqrt() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Trunc() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Clz() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Ctz() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Extend16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Extend8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Popcnt() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Clz() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Ctz() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Extend16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Extend32S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Extend8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Popcnt() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Ge() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Gt() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Le() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Lt() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Ge() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Gt() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Le() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Lt() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchI32GeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32GeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32GtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32GtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32LeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32LeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32LtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32LtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchI64GeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64GeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64GtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64GtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64LeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64LeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64LtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64LtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Add() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Copysign() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Div() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Max() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Min() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchF32Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Add() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Copysign() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Div() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Max() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Min() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchF64Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Add() override; //!< Validate the stack effect of this opcode.
	void dispatchI32And() override; //!< Validate the stack effect of this opcode.
	void dispatchI32DivS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32DivU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Or() override; //!< Validate the stack effect of this opcode.
	void dispatchI32RemS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32RemU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Rotl() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Rotr() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Shl() override; //!< Validate the stack effect of this opcode.
	void dispatchI32ShrS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32ShrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchI32Xor() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Add() override; //!< Validate the stack effect of this opcode.
	void dispatchI64And() override; //!< Validate the stack effect of this opcode.
	void dispatchI64DivS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64DivU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Or() override; //!< Validate the stack effect of this opcode.
	void dispatchI64RemS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64RemU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Rotl() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Rotr() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Shl() override; //!< Validate the stack effect of this opcode.
	void dispatchI64ShrS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64ShrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchI64Xor() override; //!< Validate the stack effect of this opcode.
	void dispatchF32ConvertI32S() override; //!< Validate the stack effect of this opcode.
	void dispatchF32ConvertI32U() override; //!< Validate the stack effect of this opcode.
	void dispatchF32ConvertI64S() override; //!< Validate the stack effect of this opcode.
	void dispatchF32ConvertI64U() override; //!< Validate the stack effect of this opcode.
	void dispatchF32DemoteF64() override; //!< Validate the stack effect of this opcode.
	void dispatchF32ReinterpretI32() override; //!< Validate the stack effect of this opcode.
	void dispatchF64ConvertI32S() override; //!< Validate the stack effect of this opcode.
	void dispatchF64ConvertI32U() override; //!< Validate the stack effect of this opcode.
	void dispatchF64ConvertI64S() override; //!< Validate the stack effect of this opcode.
	void dispatchF64ConvertI64U() override; //!< Validate the stack effect of this opcode.
	void dispatchF64PromoteF32() override; //!< Validate the stack effect of this opcode.
	void dispatchF64ReinterpretI64() override; //!< Validate the stack effect of this opcode.
	void dispatchI32ReinterpretF32() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncF32S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncF32U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncF64S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncF64U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncSatF32S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncSatF32U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncSatF64S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32TruncSatF64U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32WrapI64() override; //!< Validate the stack effect of this opcode.
	void dispatchI64ExtendI32S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64ExtendI32U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64ReinterpretF64() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncF32S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncF32U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncF64S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncF64U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncSatF32S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncSatF32U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncSatF64S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64TruncSatF64U() override; //!< Validate the stack effect of this opcode.


	//! SIMD arithmetic/comparison/conversion typing (from wasm_simd_ops.inc).
/**
 * \brief Validates the I8x16Add instruction's immediates and operand-stack effect.
 */
	void dispatchI8x16Add() override;
	void dispatchI8x16Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16MinS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16MinU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16MaxS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16MaxU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Add() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8MinS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8MinU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8MaxS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8MaxU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Add() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4MinS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4MinU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4MaxS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4MaxU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Add() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16LtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16LtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16GtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16GtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16LeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16LeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16GeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16GeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8LtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8LtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8GtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8GtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8LeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8LeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8GeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8GeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4LtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4LtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4GtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4GtU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4LeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4LeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4GeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4GeU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2LtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2GtS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2LeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2GeS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Shl() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16ShrS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16ShrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Shl() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ShrS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ShrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Shl() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ShrS() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ShrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Shl() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ShrS() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ShrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16AddSatS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16AddSatU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16SubSatS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16SubSatU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8AddSatS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8AddSatU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8SubSatS() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8SubSatU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16AvgrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8AvgrU() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Popcnt() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Q15MulRSatS() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16AllTrue() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8AllTrue() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4AllTrue() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2AllTrue() override; //!< Validate the stack effect of this opcode.
	void dispatchV128AnyTrue() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtendLowI8x16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtendHighI8x16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtendLowI8x16U() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtendHighI8x16U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtendLowI16x8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtendHighI16x8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtendLowI16x8U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtendHighI16x8U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtendLowI32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtendHighI32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtendLowI32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtendHighI32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtMulLowI8x16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtMulHighI8x16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtMulLowI8x16U() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtMulHighI8x16U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtMulLowI16x8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtMulHighI16x8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtMulLowI16x8U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtMulHighI16x8U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtMulLowI32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtMulHighI32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtMulLowI32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtMulHighI32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtAddPairwiseI8x16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtAddPairwiseI8x16U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtAddPairwiseI16x8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtAddPairwiseI16x8U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4DotI16x8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16NarrowI16x8S() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16NarrowI16x8U() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8NarrowI32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8NarrowI32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Lt() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Gt() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Le() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Ge() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Eq() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Ne() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Lt() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Gt() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Le() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Ge() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Ceil() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Floor() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Trunc() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Nearest() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Sqrt() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Ceil() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Floor() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Trunc() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Nearest() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Abs() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Neg() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Sqrt() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Add() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Div() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Min() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Max() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4PMin() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4PMax() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Add() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Sub() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Mul() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Div() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Min() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Max() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2PMin() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2PMax() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4TruncSatF32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4TruncSatF32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4ConvertI32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4ConvertI32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2ConvertLowI32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2ConvertLowI32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2PromoteLowF32x4() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4TruncSatF64x2SZero() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4TruncSatF64x2UZero() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4DemoteF64x2Zero() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4RelaxedTruncF32x4S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4RelaxedTruncF32x4U() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4RelaxedTruncF64x2SZero() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4RelaxedTruncF64x2UZero() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4RelaxedMin() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4RelaxedMax() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2RelaxedMin() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2RelaxedMax() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8RelaxedQ15MulRS() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4RelaxedMadd() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4RelaxedNmadd() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2RelaxedMadd() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2RelaxedNmadd() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8RelaxedDotI8x16I7x16S() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override; //!< Validate the stack effect of this opcode.


	//! Atomic memory accesses (operand/result typing).
	void dispatchMemoryAtomicNotify(MemArg arg) override;
	void dispatchMemoryAtomicWait32(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchMemoryAtomicWait64(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchAtomicFence(uint8_t ordering) override; //!< Validate the stack effect of this opcode.
	void dispatchAtomicPause() override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicLoad(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicLoad16U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicLoad8U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw16AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw16AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw16CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw16OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw16SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw16XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw16XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw8AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw8AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw8CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw8OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw8SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw8XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmw8XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmwAdd(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmwAnd(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmwCmpxchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmwOr(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmwSub(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmwXchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicRmwXor(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicStore(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicStore16(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32AtomicStore8(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicLoad(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicLoad16U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicLoad32U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicLoad8U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw16AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw16AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw16CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw16OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw16SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw16XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw16XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw32AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw32AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw32CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw32OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw32SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw32XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw32XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw8AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw8AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw8CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw8OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw8SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw8XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmw8XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmwAdd(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmwAnd(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmwCmpxchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmwOr(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmwSub(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmwXchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicRmwXor(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicStore(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicStore16(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicStore32(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64AtomicStore8(MemArg arg) override; //!< Validate the stack effect of this opcode.


	//! SIMD bitwise, const, bitmask and splat typing.
	void dispatchV128Const(std::span<uint8_t> arg) override;
	void dispatchV128Not() override; //!< Validate the stack effect of this opcode.
	void dispatchV128And() override; //!< Validate the stack effect of this opcode.
	void dispatchV128AndNot() override; //!< Validate the stack effect of this opcode.
	void dispatchV128Or() override; //!< Validate the stack effect of this opcode.
	void dispatchV128Xor() override; //!< Validate the stack effect of this opcode.
	void dispatchV128Bitselect() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Splat() override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8Splat() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Splat() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Splat() override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4Splat() override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2Splat() override; //!< Validate the stack effect of this opcode.

	//! Bitmask reductions are part of the wasm_simd_ops.inc shape list.
/**
 * \brief Validates the I8x16Bitmask instruction's immediates and operand-stack effect.
 */
	void dispatchI8x16Bitmask() override;
	void dispatchI16x8Bitmask() override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4Bitmask() override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2Bitmask() override; //!< Validate the stack effect of this opcode.

	void dispatchI8x16Swizzle() override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16RelaxedSwizzle() override; //!< Validate the stack effect of this opcode.

	//! SIMD memory accesses and lanes.
	void dispatchV128Load(MemArg arg) override;
	void dispatchV128Load8x8S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load8x8U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load16x4S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load16x4U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load32x2S(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load32x2U(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load8Splat(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load16Splat(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load32Splat(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load64Splat(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load32Zero(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load64Zero(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Store(MemArg arg) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load8Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load16Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load32Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Load64Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Store8Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Store16Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Store32Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchV128Store64Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16ExtractLaneS(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16ExtractLaneU(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI8x16ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtractLaneS(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ExtractLaneU(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI16x8ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI32x4ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchI64x2ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF32x4ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
	void dispatchF64x2ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
};

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMVALIDATOR_HPP

