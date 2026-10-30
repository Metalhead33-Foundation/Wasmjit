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
/**
 * \brief Performs the validator's Validator operation.
 * \param module Operand or context value used by this validator operation.
 * \param options Operand or context value used by this validator operation.
 */
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

	const Module& module;
	ValidationOptions options;
	std::size_t totalFunctions = 0;
	std::size_t totalTables = 0;
	std::size_t totalMemories = 0;
	std::size_t totalGlobals = 0;
	std::size_t totalTags = 0;

	//! Current function body (npos while validating module-level structure).
	static constexpr std::size_t kNoBody = static_cast<std::size_t>(-1);
	std::size_t currentBody = kNoBody;
	std::vector<ValueType> locals;

	//! Records a structured validation diagnostic and throws `ValidationError`
	//! with the same text. The id is `WASM.VALIDATE.<LEAF>`. An empty `id` asks
	//! `validationRuleId()` to classify the message into a stable rule family;
	//! a site that knows its rule better passes an explicit `DiagnosticId::…`.
/**
 * \brief Performs the fail operation.
 * \param message Input value or output destination for this operation.
 * \param id Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	[[noreturn]] void fail(const std::string& message,
			std::string_view id = {}) const;

	//! Diagnostic form of a local index in the body being validated
	//! (`"0 ($name)"`); the name-section outer key is the global function index.
/**
 * \brief Performs the validator's describeLocal operation.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	std::string describeLocal(uint32_t idx) const;

/**
 * \brief Performs the validator's funcTypeAt operation.
 * \param typeIdx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	FuncType funcTypeAt(uint32_t typeIdx) const;
	//! Resolve `cont $ft` to the module-local function type index it names. The
	//! canonical view stores a process-wide TypeId, so the module-local index is
	//! recovered from `typeIds` (STACK_SWITCHING.md SS-1).
/**
 * \brief Performs the validator's continuationFuncIndex operation.
 * \param contTypeIdx Operand or context value used by this validator operation.
 * \param who Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	uint32_t continuationFuncIndex(TypeIdx contTypeIdx, const char* who) const;
/**
 * \brief Performs the validator's continuationFuncType operation.
 * \param contTypeIdx Operand or context value used by this validator operation.
 * \param who Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	FuncType continuationFuncType(TypeIdx contTypeIdx, const char* who) const;
	//! The stage-2 provider carries at most one i32 parameter and one i32 result
	//! for a continuation function, a control tag and a handler payload.
/**
 * \brief Checks the MvpContinuationSignature validation precondition and reports a validation error when it is not met.
 * \param type Operand or context value used by this validator operation.
 * \param who Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireMvpContinuationSignature(const FuncType& type, const char* who) const;
/**
 * \brief Checks the FunctionIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireFunctionIndex(uint32_t idx) const;
/**
 * \brief Checks the TableIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireTableIndex(uint32_t idx) const;
/**
 * \brief Checks the MemoryIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireMemoryIndex(uint32_t idx) const;
	//! Type checks for the `string` array instructions: the type index must be
	//! an array of the required packed element type; encode variants require a
	//! mutable array.
/**
 * \brief Checks the StringArrayType validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \param packed Operand or context value used by this validator operation.
 * \param requireMutable Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireStringArrayType(TypeIdx idx, ValueTypeCode packed, bool requireMutable) const;
/**
 * \brief Checks the GlobalIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireGlobalIndex(uint32_t idx) const;
/**
 * \brief Checks the TagIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireTagIndex(uint32_t idx) const;
/**
 * \brief Checks the LocalIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireLocalIndex(uint32_t idx) const;
/**
 * \brief Checks the ElementIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireElementIndex(uint32_t idx) const;
/**
 * \brief Checks the DataIndex validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireDataIndex(uint32_t idx) const;
/**
 * \brief Returns the globalIsMutable property for the current module.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool globalIsMutable(uint32_t idx) const;
/**
 * \brief Returns the tableIsFuncRef property for the current module.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool tableIsFuncRef(uint32_t idx) const;
/**
 * \brief Returns the memoryIs64 property for the current module.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool memoryIs64(uint32_t idx) const;
/**
 * \brief Returns the memoryIsShared property for the current module.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool memoryIsShared(uint32_t idx) const;

	//! Reference subtyping (nullability + the func/extern/exn/any hierarchies).
/**
 * \brief Checks or constructs the reference-type relation required by refSubtype.
 * \param actual Operand or context value used by this validator operation.
 * \param expected Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool refSubtype(const ValueType& actual, const ValueType& expected) const;
	//! True when a local of this type has an implicit default value, so reading it
	//! before any store is valid. Non-nullable references do not.
/**
 * \brief Returns the isDefaultable property for the current module.
 * \param type Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool isDefaultable(const ValueType& type) const;
	//! `ref.func x` may only name a function that the module also mentions
	//! outside its function bodies or start function (the spec's `C.refs`).
	std::unordered_set<uint32_t> declaredFunctions;
/**
 * \brief Validates ConstExpr according to the WebAssembly validation rules.
 * \param bytes Operand or context value used by this validator operation.
 * \param label Operand or context value used by this validator operation.
 * \param globalLimit Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 * \throws ValidationError when the requested validation rule is violated.
 */
	std::optional<ValueType> validateConstExpr(const std::vector<uint8_t>& bytes,
			const std::string& label, std::size_t globalLimit);
/**
 * \brief Returns the isEqRef property for the current module.
 * \param type Operand or context value used by this validator operation.
 * \param EqRef Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool isEqRef(const ValueType& type) const {
		return refSubtype(type, ValueType{ValueTypeCode::EqRef, -1});
	}
	//! A typed reference's concrete heap type must name a declared type.
/**
 * \brief Checks the WellFormedType validation precondition and reports a validation error when it is not met.
 * \param type Operand or context value used by this validator operation.
 * \param where Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireWellFormedType(const ValueType& type, const std::string& where) const;

	//! `ref.test`/`ref.cast` target construction and the top of the target's
	//! heap hierarchy (used for the operand's same-hierarchy rule).
/**
 * \brief Checks or constructs the reference-type relation required by refType.
 * \param nullable Operand or context value used by this validator operation.
 * \param heapType Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ValueType refType(bool nullable, const HeapType& heapType) const;
/**
 * \brief Checks or constructs the reference-type relation required by refTopForHeap.
 * \param heapType Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ValueType refTopForHeap(const HeapType& heapType) const;
/**
 * \brief Validates RefTest according to the WebAssembly validation rules.
 * \param nullable Operand or context value used by this validator operation.
 * \param heapType Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateRefTest(bool nullable, const HeapType& heapType);
/**
 * \brief Validates RefCast according to the WebAssembly validation rules.
 * \param nullable Operand or context value used by this validator operation.
 * \param heapType Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateRefCast(bool nullable, const HeapType& heapType);

	//! Profile gates (docs/proposals/PROFILES.md PR-2/PR-4): reject constructs an
	//! excluded marker forbids before any structural rule reports on them.
/**
 * \brief Checks the Profile constraint for the current module.
 */
	void checkProfile() const;
/**
 * \brief Checks the ProfileValueType constraint for the current module.
 * \param type Operand or context value used by this validator operation.
 * \param where Operand or context value used by this validator operation.
 */
	void checkProfileValueType(const ValueType& type, const std::string& where) const;
	//! Prefixed-instruction gates (shared with the base dispatcher's hooks).
/**
 * \brief Performs the validator's onSimdOpcode operation.
 * \param opcode Operand or context value used by this validator operation.
 */
	void onSimdOpcode(SIMDOpcode opcode) override;
	void onAtomicOpcode(AtomicOpcode opcode) override; //!< Validate the stack effect of this opcode.

/**
 * \brief Checks the Limits constraint for the current module.
 * \param limits Operand or context value used by this validator operation.
 * \param is64 Operand or context value used by this validator operation.
 * \param isMemory Operand or context value used by this validator operation.
 * \param what Operand or context value used by this validator operation.
 * \param index Operand or context value used by this validator operation.
 * \param pageSize Operand or context value used by this validator operation.
 */
	void checkLimits(const Limits& limits, bool is64, bool isMemory,
					 const char* what, std::size_t index,
					 uint64_t pageSize = 0x10000) const;
/**
 * \brief Checks the MemoryAccess validation precondition and reports a validation error when it is not met.
 * \param arg Operand or context value used by this validator operation.
 * \param naturalLog2 Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireMemoryAccess(const MemArg& arg, uint32_t naturalLog2) const;
	//! Multibyte Array Access (GC_EXTENSIONS.md GX-1): a load/store whose memarg
	//! sets bit 5 operates on a GC array. These helpers validate the type index
	//! and element shape, enforce mutability for stores, and pop `[ref, address]`
	//! instead of the linear-memory address.
/**
 * \brief Checks the ArrayAccess validation precondition and reports a validation error when it is not met.
 * \param arg Operand or context value used by this validator operation.
 * \param naturalLog2 Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireArrayAccess(const MemArg& arg, uint32_t naturalLog2) const;
/**
 * \brief Checks the ArrayMutable validation precondition and reports a validation error when it is not met.
 * \param arg Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireArrayMutable(const MemArg& arg) const;
/**
 * \brief Consumes the expected operand-stack value or sequence, validating its type.
 * \param arg Operand or context value used by this validator operation.
 */
	void popMemoryAddress(const MemArg& arg);
	//! Atomic accesses have extra rules: the alignment immediate must equal the
	//! natural alignment, and the acquire-release ordering byte is 0 or 1.
/**
 * \brief Checks the AtomicMemoryAccess validation precondition and reports a validation error when it is not met.
 * \param arg Operand or context value used by this validator operation.
 * \param naturalLog2 Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireAtomicMemoryAccess(const MemArg& arg, uint32_t naturalLog2) const;
/**
 * \brief Checks the StructField validation precondition and reports a validation error when it is not met.
 * \param type Operand or context value used by this validator operation.
 * \param fieldIndex Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 * \throws ValidationError when the requested validation rule is violated.
 */
	const CanonFieldType& requireStructField(TypeIdx type, uint32_t fieldIndex) const;
/**
 * \brief Checks the Lane validation precondition and reports a validation error when it is not met.
 * \param lane Operand or context value used by this validator operation.
 * \param laneCount Operand or context value used by this validator operation.
 * \param what Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void requireLane(uint8_t lane, uint32_t laneCount, const char* what) const;

	//! Operand/control typing (core instructions only in this milestone).
	struct ControlFrame {
		std::vector<ValueType> startTypes;
		std::vector<ValueType> endTypes;
		std::size_t height = 0;
		bool isLoop = false;
		bool isIf = false;
		bool elseSeen = false;
		bool unreachable = false;
		//! Definite assignment of non-defaultable locals. A frame inherits its
		//! parent's set on entry; stores made inside the frame do not escape it,
		//! and `else` restarts from the entry set (the spec's local-init rule).
		std::vector<uint8_t> initialized;
		std::vector<uint8_t> entryInitialized;
	};
	std::vector<ValueType> typeStack;
	std::vector<ControlFrame> controlStack;
	//! Set when an opcode without a typing rule is seen: the rest of the body is
	//! still scanned for immediate checks, but the operand stack is not trusted.
	bool typeTainted = false;

/**
 * \brief Resolves frame from the current function and module type context.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ControlFrame& frame();
/**
 * \brief Resolves blockParams from the current function and module type context.
 * \param blockType Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	std::vector<ValueType> blockParams(const BlockType& blockType) const;
/**
 * \brief Resolves blockResults from the current function and module type context.
 * \param blockType Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	std::vector<ValueType> blockResults(const BlockType& blockType) const;
/**
 * \brief Adds the specified value type or type sequence to the validation operand stack.
 * \param type Operand or context value used by this validator operation.
 */
	void pushValue(const ValueType& type);
/**
 * \brief Consumes the expected operand-stack value or sequence, validating its type.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	std::optional<ValueType> popAny();
/**
 * \brief Consumes the expected operand-stack value or sequence, validating its type.
 * \param code Operand or context value used by this validator operation.
 */
	void popExpect(ValueTypeCode code);
/**
 * \brief Consumes the expected operand-stack value or sequence, validating its type.
 * \param type Operand or context value used by this validator operation.
 */
	void popExpect(const ValueType& type);
/**
 * \brief Adds the specified value type or type sequence to the validation operand stack.
 * \param types Operand or context value used by this validator operation.
 */
	void pushTypes(const std::vector<ValueType>& types);
/**
 * \brief Consumes the expected operand-stack value or sequence, validating its type.
 * \param types Operand or context value used by this validator operation.
 */
	void popTypes(const std::vector<ValueType>& types);
/**
 * \brief Checks the TailCallResults constraint for the current module.
 * \param callee Operand or context value used by this validator operation.
 */
	void checkTailCallResults(const FuncType& callee);
/**
 * \brief Performs the validator's assignable operation.
 * \param actual Operand or context value used by this validator operation.
 * \param expected Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool assignable(const ValueType& actual, const ValueType& expected) const;
	//! True for `cont`, `nocont` and a concrete `cont $ft` type index. The
	//! stack-switching proposal forbids casting to any continuation type.
/**
 * \brief Returns the isContinuationHeap property for the current module.
 * \param type Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool isContinuationHeap(const HeapType& type) const;
	//! More Array Constructors (GC_EXTENSIONS.md GX-2) helpers.
/**
 * \brief Checks the ArrayType validation precondition and reports a validation error when it is not met.
 * \param idx Operand or context value used by this validator operation.
 * \param who Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 * \throws ValidationError when the requested validation rule is violated.
 */
	CoreTypeView requireArrayType(TypeIdx idx, const char* who) const;
/**
 * \brief Performs the validator's storageAssignable operation.
 * \param actual Operand or context value used by this validator operation.
 * \param expected Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	bool storageAssignable(const CanonStorageType& actual,
			const CanonStorageType& expected) const;
/**
 * \brief Performs the validator's setUnreachable operation.
 */
	void setUnreachable();
/**
 * \brief Performs the validator's beginBlock operation.
 * \param blockType Operand or context value used by this validator operation.
 * \param isLoop Operand or context value used by this validator operation.
 * \param isIf Operand or context value used by this validator operation.
 */
	void beginBlock(const BlockType& blockType, bool isLoop, bool isIf);
/**
 * \brief Performs the validator's endBlock operation.
 * \param isElse Operand or context value used by this validator operation.
 */
	void endBlock(bool isElse);
/**
 * \brief Performs the validator's branch operation.
 * \param label Operand or context value used by this validator operation.
 */
	void branch(LabelIdx label);
/**
 * \brief Performs the validator's branchIf operation.
 * \param label Operand or context value used by this validator operation.
 */
	void branchIf(LabelIdx label);
/**
 * \brief Returns the memoryAddressType property for the current module.
 * \param memIdx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ValueTypeCode memoryAddressType(MemIdx memIdx) const;
/**
 * \brief Returns the tableIndexType property for the current module.
 * \param tableIdx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ValueTypeCode tableIndexType(TableIdx tableIdx) const;
/**
 * \brief Resolves functionType from the current function and module type context.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	FuncType functionType(FuncIdx idx) const;
/**
 * \brief Returns the globalType property for the current module.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ValueType globalType(GlobalIdx idx) const;
/**
 * \brief Returns the tableElementType property for the current module.
 * \param idx Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	ValueType tableElementType(TableIdx idx) const;
	const FuncType* currentFuncType = nullptr;
/**
 * \brief Resolves paramTypes from the current function and module type context.
 * \param types Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
	std::vector<ValueType> paramTypes(const FuncType& types) const;
/**
 * \brief Resolves resultTypes from the current function and module type context.
 * \param types Operand or context value used by this validator operation.
 * \return The resolved value, predicate result, or validation data described by the function name.
 */
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
/**
 * \brief Performs the dispatchI32Const operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32Const(int32_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Const operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64Const(int64_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Const operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchF32Const(float arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Const operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchF64Const(double arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchRefIsNull operation.
 */
	void dispatchRefIsNull() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchRefEq operation.
 */
	void dispatchRefEq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchRefAsNonNull operation.
 */
	void dispatchRefAsNonNull() override; //!< Validate the stack effect of this opcode.

	//! More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2).
/**
 * \brief Validates the ArrayNewArray instruction's immediates and operand-stack effect.
 * \param dst Operand or context value used by this validator operation.
 * \param src Operand or context value used by this validator operation.
 */
	void dispatchArrayNewArray(TypeIdx dst, TypeIdx src) override;
	void dispatchArrayNewMemory(TypeIdx dst, MemIdx src) override; //!< Validate the stack effect of this opcode.
	void dispatchArrayNewTable(TypeIdx dst, TableIdx src) override; //!< Validate the stack effect of this opcode.
	//! Custom Descriptors (0xFB 32..38).
/**
 * \brief Validates the StructNew instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 */
	void dispatchStructNew(TypeIdx arg) override;
	void dispatchStructNewDefault(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStructNewDesc(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchStructNewDefaultDesc(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefGetDesc(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefCastDescEq(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchRefCastDescEqNull(const HeapType& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCastDescEq(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCastDescEqFail(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Validates CastDescEq according to the WebAssembly validation rules.
 * \param nullableTarget Operand or context value used by this validator operation.
 * \param target Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateCastDescEq(bool nullableTarget, const HeapType& target);
/**
 * \brief Validates BrOnCastDescEq according to the WebAssembly validation rules.
 * \param isFail Operand or context value used by this validator operation.
 * \param castop Operand or context value used by this validator operation.
 * \param label Operand or context value used by this validator operation.
 * \param ht1 Operand or context value used by this validator operation.
 * \param ht2 Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateBrOnCastDescEq(bool isFail, uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2);

/**
 * \brief Validates Module according to the WebAssembly validation rules.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateModule();
/**
 * \brief Validates GlobalInit according to the WebAssembly validation rules.
 * \param global Operand or context value used by this validator operation.
 * \param index Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateGlobalInit(const Global& global, std::size_t index);
/**
 * \brief Validates ElementSegment according to the WebAssembly validation rules.
 * \param segment Operand or context value used by this validator operation.
 * \param index Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateElementSegment(const ElementSegment& segment, std::size_t index);
/**
 * \brief Validates DataSegment according to the WebAssembly validation rules.
 * \param segment Operand or context value used by this validator operation.
 * \param index Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateDataSegment(const DataSegment& segment, std::size_t index);
/**
 * \brief Validates TableInit according to the WebAssembly validation rules.
 * \param table Operand or context value used by this validator operation.
 * \param index Operand or context value used by this validator operation.
 * \param isImport Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateTableInit(const TableType& table, std::size_t index, bool isImport = false);
/**
 * \brief Validates FunctionBody according to the WebAssembly validation rules.
 * \param body Operand or context value used by this validator operation.
 * \param index Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
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
/**
 * \brief Performs the dispatchStringAsWtf8 operation.
 */
	void dispatchStringAsWtf8() override;
/**
 * \brief Performs the dispatchStringViewWtf8Advance operation.
 */
	void dispatchStringViewWtf8Advance() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf8EncodeUtf8 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchStringViewWtf8EncodeUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf8Slice operation.
 */
	void dispatchStringViewWtf8Slice() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf8EncodeLossyUtf8 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchStringViewWtf8EncodeLossyUtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf8EncodeWtf8 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchStringViewWtf8EncodeWtf8(MemIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringAsWtf16 operation.
 */
	void dispatchStringAsWtf16() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf16Length operation.
 */
	void dispatchStringViewWtf16Length() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf16GetCodeUnit operation.
 */
	void dispatchStringViewWtf16GetCodeUnit() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf16Encode operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchStringViewWtf16Encode(MemIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewWtf16Slice operation.
 */
	void dispatchStringViewWtf16Slice() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringAsIter operation.
 */
	void dispatchStringAsIter() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewIterNext operation.
 */
	void dispatchStringViewIterNext() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewIterAdvance operation.
 */
	void dispatchStringViewIterAdvance() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewIterRewind operation.
 */
	void dispatchStringViewIterRewind() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStringViewIterSlice operation.
 */
	void dispatchStringViewIterSlice() override; //!< Validate the stack effect of this opcode.

	//! GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
/**
 * \brief Validates the StringNewUtf8Array instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 */
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
/**
 * \brief Validates the LocalGet instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 */
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
/**
 * \brief Performs the dispatchContNew operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchContNew(TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchContBind operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchContBind(TypeIdx arg1, TypeIdx arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchSuspend operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchSuspend(TagIdx arg) override; //!< Validate the stack effect of this opcode.
	//! Shared validation for `resume`/`resume_throw*` handler clauses.
/**
 * \brief Validates ContHandlers according to the WebAssembly validation rules.
 * \param handlers Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
	void validateContHandlers(const std::vector<ContHandlerClause>& handlers);
	void dispatchResume(TypeIdx arg, std::vector<ContHandlerClause>&& handlers) override; //!< Validate the stack effect of this opcode.
	//! Dispatch the `ResumeThrow` instruction. Validate the stack effect of this opcode.
/**
 * \brief Validates the ResumeThrow instruction's immediates and operand-stack effect.
 * \param arg1 Operand or context value used by this validator operation.
 * \param arg2 Operand or context value used by this validator operation.
 * \param handlers Operand or context value used by this validator operation.
 */
	void dispatchResumeThrow(TypeIdx arg1, TagIdx arg2,
			std::vector<ContHandlerClause>&& handlers) override;
	//! Dispatch the `ResumeThrowRef` instruction. Validate the stack effect of this opcode.
/**
 * \brief Validates the ResumeThrowRef instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 * \param handlers Operand or context value used by this validator operation.
 */
	void dispatchResumeThrowRef(TypeIdx arg,
			std::vector<ContHandlerClause>&& handlers) override;
	void dispatchSwitch(TypeIdx arg1, TagIdx arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchTryTable(const BlockType& arg1, std::vector<CatchClause>&& arg2) override; //!< Validate the stack effect of this opcode.
	void dispatchSelectT(std::vector<ValueType>&& arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnNull(LabelIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnNonNull(LabelIdx arg) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCast(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
	void dispatchBrOnCastFail(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Validates BrOnCast according to the WebAssembly validation rules.
 * \param isFail Operand or context value used by this validator operation.
 * \param castop Operand or context value used by this validator operation.
 * \param label Operand or context value used by this validator operation.
 * \param ht1 Operand or context value used by this validator operation.
 * \param ht2 Operand or context value used by this validator operation.
 * \throws ValidationError when the requested validation rule is violated.
 */
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
/**
 * \brief Performs the dispatchRoundingVariant operation.
 * \param sub Input value or output destination for this operation.
 */
	void dispatchRoundingVariant(uint32_t sub) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Add128 operation.
 */
	void dispatchI64Add128() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Sub128 operation.
 */
	void dispatchI64Sub128() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64MulWideS operation.
 */
	void dispatchI64MulWideS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64MulWideU operation.
 */
	void dispatchI64MulWideU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Itt operation.
 * \param immediate Input value or output destination for this operation.
 */
	void dispatchI32Itt(uint32_t immediate) override; //!< Validate the stack effect of this opcode.

	//! Core memory accesses.
/**
 * \brief Validates the I32Load instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 */
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
/**
 * \brief Performs the dispatchI32Store16 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32Store16(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Store8 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64Store8(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Store16 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64Store16(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Store32 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64Store32(MemArg arg) override; //!< Validate the stack effect of this opcode.


	//! Core numeric instructions (operand/result typing).
/**
 * \brief Validates the I32Eqz instruction's immediates and operand-stack effect.
 */
/**
 * \brief Performs the dispatchI32Eqz operation.
 */
	void dispatchI32Eqz() override;
/**
 * \brief Performs the dispatchI64Eqz operation.
 */
	void dispatchI64Eqz() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Abs operation.
 */
	void dispatchF32Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Ceil operation.
 */
	void dispatchF32Ceil() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Floor operation.
 */
	void dispatchF32Floor() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Nearest operation.
 */
	void dispatchF32Nearest() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Neg operation.
 */
	void dispatchF32Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Sqrt operation.
 */
	void dispatchF32Sqrt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Trunc operation.
 */
	void dispatchF32Trunc() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Abs operation.
 */
	void dispatchF64Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Ceil operation.
 */
	void dispatchF64Ceil() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Floor operation.
 */
	void dispatchF64Floor() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Nearest operation.
 */
	void dispatchF64Nearest() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Neg operation.
 */
	void dispatchF64Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Sqrt operation.
 */
	void dispatchF64Sqrt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Trunc operation.
 */
	void dispatchF64Trunc() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Clz operation.
 */
	void dispatchI32Clz() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Ctz operation.
 */
	void dispatchI32Ctz() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Extend16S operation.
 */
	void dispatchI32Extend16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Extend8S operation.
 */
	void dispatchI32Extend8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Popcnt operation.
 */
	void dispatchI32Popcnt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Clz operation.
 */
	void dispatchI64Clz() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Ctz operation.
 */
	void dispatchI64Ctz() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Extend16S operation.
 */
	void dispatchI64Extend16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Extend32S operation.
 */
	void dispatchI64Extend32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Extend8S operation.
 */
	void dispatchI64Extend8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Popcnt operation.
 */
	void dispatchI64Popcnt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Eq operation.
 */
	void dispatchF32Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Ge operation.
 */
	void dispatchF32Ge() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Gt operation.
 */
	void dispatchF32Gt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Le operation.
 */
	void dispatchF32Le() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Lt operation.
 */
	void dispatchF32Lt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Ne operation.
 */
	void dispatchF32Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Eq operation.
 */
	void dispatchF64Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Ge operation.
 */
	void dispatchF64Ge() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Gt operation.
 */
	void dispatchF64Gt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Le operation.
 */
	void dispatchF64Le() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Lt operation.
 */
	void dispatchF64Lt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Ne operation.
 */
	void dispatchF64Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Eq operation.
 */
	void dispatchI32Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32GeS operation.
 */
	void dispatchI32GeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32GeU operation.
 */
	void dispatchI32GeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32GtS operation.
 */
	void dispatchI32GtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32GtU operation.
 */
	void dispatchI32GtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32LeS operation.
 */
	void dispatchI32LeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32LeU operation.
 */
	void dispatchI32LeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32LtS operation.
 */
	void dispatchI32LtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32LtU operation.
 */
	void dispatchI32LtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Ne operation.
 */
	void dispatchI32Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Eq operation.
 */
	void dispatchI64Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64GeS operation.
 */
	void dispatchI64GeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64GeU operation.
 */
	void dispatchI64GeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64GtS operation.
 */
	void dispatchI64GtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64GtU operation.
 */
	void dispatchI64GtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64LeS operation.
 */
	void dispatchI64LeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64LeU operation.
 */
	void dispatchI64LeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64LtS operation.
 */
	void dispatchI64LtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64LtU operation.
 */
	void dispatchI64LtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Ne operation.
 */
	void dispatchI64Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Add operation.
 */
	void dispatchF32Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Copysign operation.
 */
	void dispatchF32Copysign() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Div operation.
 */
	void dispatchF32Div() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Max operation.
 */
	void dispatchF32Max() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Min operation.
 */
	void dispatchF32Min() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Mul operation.
 */
	void dispatchF32Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32Sub operation.
 */
	void dispatchF32Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Add operation.
 */
	void dispatchF64Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Copysign operation.
 */
	void dispatchF64Copysign() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Div operation.
 */
	void dispatchF64Div() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Max operation.
 */
	void dispatchF64Max() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Min operation.
 */
	void dispatchF64Min() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Mul operation.
 */
	void dispatchF64Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64Sub operation.
 */
	void dispatchF64Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Add operation.
 */
	void dispatchI32Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32And operation.
 */
	void dispatchI32And() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32DivS operation.
 */
	void dispatchI32DivS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32DivU operation.
 */
	void dispatchI32DivU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Mul operation.
 */
	void dispatchI32Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Or operation.
 */
	void dispatchI32Or() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32RemS operation.
 */
	void dispatchI32RemS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32RemU operation.
 */
	void dispatchI32RemU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Rotl operation.
 */
	void dispatchI32Rotl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Rotr operation.
 */
	void dispatchI32Rotr() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Shl operation.
 */
	void dispatchI32Shl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32ShrS operation.
 */
	void dispatchI32ShrS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32ShrU operation.
 */
	void dispatchI32ShrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Sub operation.
 */
	void dispatchI32Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32Xor operation.
 */
	void dispatchI32Xor() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Add operation.
 */
	void dispatchI64Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64And operation.
 */
	void dispatchI64And() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64DivS operation.
 */
	void dispatchI64DivS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64DivU operation.
 */
	void dispatchI64DivU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Mul operation.
 */
	void dispatchI64Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Or operation.
 */
	void dispatchI64Or() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64RemS operation.
 */
	void dispatchI64RemS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64RemU operation.
 */
	void dispatchI64RemU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Rotl operation.
 */
	void dispatchI64Rotl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Rotr operation.
 */
	void dispatchI64Rotr() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Shl operation.
 */
	void dispatchI64Shl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64ShrS operation.
 */
	void dispatchI64ShrS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64ShrU operation.
 */
	void dispatchI64ShrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Sub operation.
 */
	void dispatchI64Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64Xor operation.
 */
	void dispatchI64Xor() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32ConvertI32S operation.
 */
	void dispatchF32ConvertI32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32ConvertI32U operation.
 */
	void dispatchF32ConvertI32U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32ConvertI64S operation.
 */
	void dispatchF32ConvertI64S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32ConvertI64U operation.
 */
	void dispatchF32ConvertI64U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32DemoteF64 operation.
 */
	void dispatchF32DemoteF64() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32ReinterpretI32 operation.
 */
	void dispatchF32ReinterpretI32() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64ConvertI32S operation.
 */
	void dispatchF64ConvertI32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64ConvertI32U operation.
 */
	void dispatchF64ConvertI32U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64ConvertI64S operation.
 */
	void dispatchF64ConvertI64S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64ConvertI64U operation.
 */
	void dispatchF64ConvertI64U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64PromoteF32 operation.
 */
	void dispatchF64PromoteF32() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64ReinterpretI64 operation.
 */
	void dispatchF64ReinterpretI64() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32ReinterpretF32 operation.
 */
	void dispatchI32ReinterpretF32() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncF32S operation.
 */
	void dispatchI32TruncF32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncF32U operation.
 */
	void dispatchI32TruncF32U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncF64S operation.
 */
	void dispatchI32TruncF64S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncF64U operation.
 */
	void dispatchI32TruncF64U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncSatF32S operation.
 */
	void dispatchI32TruncSatF32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncSatF32U operation.
 */
	void dispatchI32TruncSatF32U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncSatF64S operation.
 */
	void dispatchI32TruncSatF64S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32TruncSatF64U operation.
 */
	void dispatchI32TruncSatF64U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32WrapI64 operation.
 */
	void dispatchI32WrapI64() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64ExtendI32S operation.
 */
	void dispatchI64ExtendI32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64ExtendI32U operation.
 */
	void dispatchI64ExtendI32U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64ReinterpretF64 operation.
 */
	void dispatchI64ReinterpretF64() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncF32S operation.
 */
	void dispatchI64TruncF32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncF32U operation.
 */
	void dispatchI64TruncF32U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncF64S operation.
 */
	void dispatchI64TruncF64S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncF64U operation.
 */
	void dispatchI64TruncF64U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncSatF32S operation.
 */
	void dispatchI64TruncSatF32S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncSatF32U operation.
 */
	void dispatchI64TruncSatF32U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncSatF64S operation.
 */
	void dispatchI64TruncSatF64S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64TruncSatF64U operation.
 */
	void dispatchI64TruncSatF64U() override; //!< Validate the stack effect of this opcode.


	//! SIMD arithmetic/comparison/conversion typing (from wasm_simd_ops.inc).
/**
 * \brief Validates the I8x16Add instruction's immediates and operand-stack effect.
 */
/**
 * \brief Performs the dispatchI8x16Add operation.
 */
	void dispatchI8x16Add() override;
/**
 * \brief Performs the dispatchI8x16Sub operation.
 */
	void dispatchI8x16Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16MinS operation.
 */
	void dispatchI8x16MinS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16MinU operation.
 */
	void dispatchI8x16MinU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16MaxS operation.
 */
	void dispatchI8x16MaxS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16MaxU operation.
 */
	void dispatchI8x16MaxU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Add operation.
 */
	void dispatchI16x8Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Sub operation.
 */
	void dispatchI16x8Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Mul operation.
 */
	void dispatchI16x8Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8MinS operation.
 */
	void dispatchI16x8MinS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8MinU operation.
 */
	void dispatchI16x8MinU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8MaxS operation.
 */
	void dispatchI16x8MaxS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8MaxU operation.
 */
	void dispatchI16x8MaxU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Add operation.
 */
	void dispatchI32x4Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Sub operation.
 */
	void dispatchI32x4Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Mul operation.
 */
	void dispatchI32x4Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4MinS operation.
 */
	void dispatchI32x4MinS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4MinU operation.
 */
	void dispatchI32x4MinU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4MaxS operation.
 */
	void dispatchI32x4MaxS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4MaxU operation.
 */
	void dispatchI32x4MaxU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Add operation.
 */
	void dispatchI64x2Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Sub operation.
 */
	void dispatchI64x2Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Mul operation.
 */
	void dispatchI64x2Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16Eq operation.
 */
	void dispatchI8x16Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16Ne operation.
 */
	void dispatchI8x16Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16LtS operation.
 */
	void dispatchI8x16LtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16LtU operation.
 */
	void dispatchI8x16LtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16GtS operation.
 */
	void dispatchI8x16GtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16GtU operation.
 */
	void dispatchI8x16GtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16LeS operation.
 */
	void dispatchI8x16LeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16LeU operation.
 */
	void dispatchI8x16LeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16GeS operation.
 */
	void dispatchI8x16GeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16GeU operation.
 */
	void dispatchI8x16GeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Eq operation.
 */
	void dispatchI16x8Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Ne operation.
 */
	void dispatchI16x8Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8LtS operation.
 */
	void dispatchI16x8LtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8LtU operation.
 */
	void dispatchI16x8LtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8GtS operation.
 */
	void dispatchI16x8GtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8GtU operation.
 */
	void dispatchI16x8GtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8LeS operation.
 */
	void dispatchI16x8LeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8LeU operation.
 */
	void dispatchI16x8LeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8GeS operation.
 */
	void dispatchI16x8GeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8GeU operation.
 */
	void dispatchI16x8GeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Eq operation.
 */
	void dispatchI32x4Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Ne operation.
 */
	void dispatchI32x4Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4LtS operation.
 */
	void dispatchI32x4LtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4LtU operation.
 */
	void dispatchI32x4LtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4GtS operation.
 */
	void dispatchI32x4GtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4GtU operation.
 */
	void dispatchI32x4GtU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4LeS operation.
 */
	void dispatchI32x4LeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4LeU operation.
 */
	void dispatchI32x4LeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4GeS operation.
 */
	void dispatchI32x4GeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4GeU operation.
 */
	void dispatchI32x4GeU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Eq operation.
 */
	void dispatchI64x2Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Ne operation.
 */
	void dispatchI64x2Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2LtS operation.
 */
	void dispatchI64x2LtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2GtS operation.
 */
	void dispatchI64x2GtS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2LeS operation.
 */
	void dispatchI64x2LeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2GeS operation.
 */
	void dispatchI64x2GeS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16Neg operation.
 */
	void dispatchI8x16Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Neg operation.
 */
	void dispatchI16x8Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Neg operation.
 */
	void dispatchI32x4Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Neg operation.
 */
	void dispatchI64x2Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16Abs operation.
 */
	void dispatchI8x16Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Abs operation.
 */
	void dispatchI16x8Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Abs operation.
 */
	void dispatchI32x4Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Abs operation.
 */
	void dispatchI64x2Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16Shl operation.
 */
	void dispatchI8x16Shl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16ShrS operation.
 */
	void dispatchI8x16ShrS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16ShrU operation.
 */
	void dispatchI8x16ShrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Shl operation.
 */
	void dispatchI16x8Shl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ShrS operation.
 */
	void dispatchI16x8ShrS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ShrU operation.
 */
	void dispatchI16x8ShrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Shl operation.
 */
	void dispatchI32x4Shl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ShrS operation.
 */
	void dispatchI32x4ShrS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ShrU operation.
 */
	void dispatchI32x4ShrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Shl operation.
 */
	void dispatchI64x2Shl() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ShrS operation.
 */
	void dispatchI64x2ShrS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ShrU operation.
 */
	void dispatchI64x2ShrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16AddSatS operation.
 */
	void dispatchI8x16AddSatS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16AddSatU operation.
 */
	void dispatchI8x16AddSatU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16SubSatS operation.
 */
	void dispatchI8x16SubSatS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16SubSatU operation.
 */
	void dispatchI8x16SubSatU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8AddSatS operation.
 */
	void dispatchI16x8AddSatS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8AddSatU operation.
 */
	void dispatchI16x8AddSatU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8SubSatS operation.
 */
	void dispatchI16x8SubSatS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8SubSatU operation.
 */
	void dispatchI16x8SubSatU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16AvgrU operation.
 */
	void dispatchI8x16AvgrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8AvgrU operation.
 */
	void dispatchI16x8AvgrU() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16Popcnt operation.
 */
	void dispatchI8x16Popcnt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8Q15MulRSatS operation.
 */
	void dispatchI16x8Q15MulRSatS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16AllTrue operation.
 */
	void dispatchI8x16AllTrue() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8AllTrue operation.
 */
	void dispatchI16x8AllTrue() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4AllTrue operation.
 */
	void dispatchI32x4AllTrue() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2AllTrue operation.
 */
	void dispatchI64x2AllTrue() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchV128AnyTrue operation.
 */
	void dispatchV128AnyTrue() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtendLowI8x16S operation.
 */
	void dispatchI16x8ExtendLowI8x16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtendHighI8x16S operation.
 */
	void dispatchI16x8ExtendHighI8x16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtendLowI8x16U operation.
 */
	void dispatchI16x8ExtendLowI8x16U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtendHighI8x16U operation.
 */
	void dispatchI16x8ExtendHighI8x16U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtendLowI16x8S operation.
 */
	void dispatchI32x4ExtendLowI16x8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtendHighI16x8S operation.
 */
	void dispatchI32x4ExtendHighI16x8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtendLowI16x8U operation.
 */
	void dispatchI32x4ExtendLowI16x8U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtendHighI16x8U operation.
 */
	void dispatchI32x4ExtendHighI16x8U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtendLowI32x4S operation.
 */
	void dispatchI64x2ExtendLowI32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtendHighI32x4S operation.
 */
	void dispatchI64x2ExtendHighI32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtendLowI32x4U operation.
 */
	void dispatchI64x2ExtendLowI32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtendHighI32x4U operation.
 */
	void dispatchI64x2ExtendHighI32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtMulLowI8x16S operation.
 */
	void dispatchI16x8ExtMulLowI8x16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtMulHighI8x16S operation.
 */
	void dispatchI16x8ExtMulHighI8x16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtMulLowI8x16U operation.
 */
	void dispatchI16x8ExtMulLowI8x16U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtMulHighI8x16U operation.
 */
	void dispatchI16x8ExtMulHighI8x16U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtMulLowI16x8S operation.
 */
	void dispatchI32x4ExtMulLowI16x8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtMulHighI16x8S operation.
 */
	void dispatchI32x4ExtMulHighI16x8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtMulLowI16x8U operation.
 */
	void dispatchI32x4ExtMulLowI16x8U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtMulHighI16x8U operation.
 */
	void dispatchI32x4ExtMulHighI16x8U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtMulLowI32x4S operation.
 */
	void dispatchI64x2ExtMulLowI32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtMulHighI32x4S operation.
 */
	void dispatchI64x2ExtMulHighI32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtMulLowI32x4U operation.
 */
	void dispatchI64x2ExtMulLowI32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtMulHighI32x4U operation.
 */
	void dispatchI64x2ExtMulHighI32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtAddPairwiseI8x16S operation.
 */
	void dispatchI16x8ExtAddPairwiseI8x16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtAddPairwiseI8x16U operation.
 */
	void dispatchI16x8ExtAddPairwiseI8x16U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtAddPairwiseI16x8S operation.
 */
	void dispatchI32x4ExtAddPairwiseI16x8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtAddPairwiseI16x8U operation.
 */
	void dispatchI32x4ExtAddPairwiseI16x8U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4DotI16x8S operation.
 */
	void dispatchI32x4DotI16x8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16NarrowI16x8S operation.
 */
	void dispatchI8x16NarrowI16x8S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16NarrowI16x8U operation.
 */
	void dispatchI8x16NarrowI16x8U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8NarrowI32x4S operation.
 */
	void dispatchI16x8NarrowI32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8NarrowI32x4U operation.
 */
	void dispatchI16x8NarrowI32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Eq operation.
 */
	void dispatchF32x4Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Ne operation.
 */
	void dispatchF32x4Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Lt operation.
 */
	void dispatchF32x4Lt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Gt operation.
 */
	void dispatchF32x4Gt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Le operation.
 */
	void dispatchF32x4Le() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Ge operation.
 */
	void dispatchF32x4Ge() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Eq operation.
 */
	void dispatchF64x2Eq() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Ne operation.
 */
	void dispatchF64x2Ne() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Lt operation.
 */
	void dispatchF64x2Lt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Gt operation.
 */
	void dispatchF64x2Gt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Le operation.
 */
	void dispatchF64x2Le() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Ge operation.
 */
	void dispatchF64x2Ge() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Ceil operation.
 */
	void dispatchF32x4Ceil() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Floor operation.
 */
	void dispatchF32x4Floor() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Trunc operation.
 */
	void dispatchF32x4Trunc() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Nearest operation.
 */
	void dispatchF32x4Nearest() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Abs operation.
 */
	void dispatchF32x4Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Neg operation.
 */
	void dispatchF32x4Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Sqrt operation.
 */
	void dispatchF32x4Sqrt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Ceil operation.
 */
	void dispatchF64x2Ceil() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Floor operation.
 */
	void dispatchF64x2Floor() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Trunc operation.
 */
	void dispatchF64x2Trunc() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Nearest operation.
 */
	void dispatchF64x2Nearest() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Abs operation.
 */
	void dispatchF64x2Abs() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Neg operation.
 */
	void dispatchF64x2Neg() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Sqrt operation.
 */
	void dispatchF64x2Sqrt() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Add operation.
 */
	void dispatchF32x4Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Sub operation.
 */
	void dispatchF32x4Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Mul operation.
 */
	void dispatchF32x4Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Div operation.
 */
	void dispatchF32x4Div() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Min operation.
 */
	void dispatchF32x4Min() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4Max operation.
 */
	void dispatchF32x4Max() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4PMin operation.
 */
	void dispatchF32x4PMin() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4PMax operation.
 */
	void dispatchF32x4PMax() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Add operation.
 */
	void dispatchF64x2Add() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Sub operation.
 */
	void dispatchF64x2Sub() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Mul operation.
 */
	void dispatchF64x2Mul() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Div operation.
 */
	void dispatchF64x2Div() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Min operation.
 */
	void dispatchF64x2Min() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2Max operation.
 */
	void dispatchF64x2Max() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2PMin operation.
 */
	void dispatchF64x2PMin() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2PMax operation.
 */
	void dispatchF64x2PMax() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4TruncSatF32x4S operation.
 */
	void dispatchI32x4TruncSatF32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4TruncSatF32x4U operation.
 */
	void dispatchI32x4TruncSatF32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4ConvertI32x4S operation.
 */
	void dispatchF32x4ConvertI32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4ConvertI32x4U operation.
 */
	void dispatchF32x4ConvertI32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2ConvertLowI32x4S operation.
 */
	void dispatchF64x2ConvertLowI32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2ConvertLowI32x4U operation.
 */
	void dispatchF64x2ConvertLowI32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2PromoteLowF32x4 operation.
 */
	void dispatchF64x2PromoteLowF32x4() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4TruncSatF64x2SZero operation.
 */
	void dispatchI32x4TruncSatF64x2SZero() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4TruncSatF64x2UZero operation.
 */
	void dispatchI32x4TruncSatF64x2UZero() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4DemoteF64x2Zero operation.
 */
	void dispatchF32x4DemoteF64x2Zero() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4RelaxedTruncF32x4S operation.
 */
	void dispatchI32x4RelaxedTruncF32x4S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4RelaxedTruncF32x4U operation.
 */
	void dispatchI32x4RelaxedTruncF32x4U() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4RelaxedTruncF64x2SZero operation.
 */
	void dispatchI32x4RelaxedTruncF64x2SZero() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4RelaxedTruncF64x2UZero operation.
 */
	void dispatchI32x4RelaxedTruncF64x2UZero() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4RelaxedMin operation.
 */
	void dispatchF32x4RelaxedMin() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4RelaxedMax operation.
 */
	void dispatchF32x4RelaxedMax() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2RelaxedMin operation.
 */
	void dispatchF64x2RelaxedMin() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2RelaxedMax operation.
 */
	void dispatchF64x2RelaxedMax() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8RelaxedQ15MulRS operation.
 */
	void dispatchI16x8RelaxedQ15MulRS() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4RelaxedMadd operation.
 */
	void dispatchF32x4RelaxedMadd() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4RelaxedNmadd operation.
 */
	void dispatchF32x4RelaxedNmadd() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2RelaxedMadd operation.
 */
	void dispatchF64x2RelaxedMadd() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2RelaxedNmadd operation.
 */
	void dispatchF64x2RelaxedNmadd() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16RelaxedLaneselect operation.
 */
	void dispatchI8x16RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8RelaxedLaneselect operation.
 */
	void dispatchI16x8RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4RelaxedLaneselect operation.
 */
	void dispatchI32x4RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2RelaxedLaneselect operation.
 */
	void dispatchI64x2RelaxedLaneselect() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8RelaxedDotI8x16I7x16S operation.
 */
	void dispatchI16x8RelaxedDotI8x16I7x16S() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4RelaxedDotI8x16I7x16AddS operation.
 */
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override; //!< Validate the stack effect of this opcode.


	//! Atomic memory accesses (operand/result typing).
/**
 * \brief Validates the MemoryAtomicNotify instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 */
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
/**
 * \brief Performs the dispatchStructAtomicGetS operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicGetU operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicSet operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicRmwAdd operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicRmwSub operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicRmwAnd operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicRmwOr operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicRmwXor operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicRmwXchg operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchStructAtomicRmwCmpxchg operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicGet operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicGetS operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicGetU operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicSet operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicRmwAdd operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicRmwSub operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicRmwAnd operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicRmwOr operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicRmwXor operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicRmwXchg operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchArrayAtomicRmwCmpxchg operation.
 * \param ordering Input value or output destination for this operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicLoad operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicLoad(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicLoad16U operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicLoad16U(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicLoad8U operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicLoad8U(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw16AddU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw16AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw16AndU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw16AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw16CmpxchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw16CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw16OrU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw16OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw16SubU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw16SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw16XchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw16XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw16XorU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw16XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw8AddU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw8AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw8AndU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw8AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw8CmpxchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw8CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw8OrU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw8OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw8SubU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw8SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw8XchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw8XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmw8XorU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmw8XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmwAdd operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmwAdd(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmwAnd operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmwAnd(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmwCmpxchg operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmwCmpxchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmwOr operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmwOr(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmwSub operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmwSub(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmwXchg operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmwXchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicRmwXor operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicRmwXor(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicStore operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicStore(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicStore16 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicStore16(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32AtomicStore8 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32AtomicStore8(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicLoad operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicLoad(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicLoad16U operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicLoad16U(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicLoad32U operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicLoad32U(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicLoad8U operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicLoad8U(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw16AddU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw16AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw16AndU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw16AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw16CmpxchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw16CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw16OrU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw16OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw16SubU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw16SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw16XchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw16XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw16XorU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw16XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw32AddU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw32AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw32AndU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw32AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw32CmpxchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw32CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw32OrU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw32OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw32SubU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw32SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw32XchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw32XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw32XorU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw32XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw8AddU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw8AddU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw8AndU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw8AndU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw8CmpxchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw8CmpxchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw8OrU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw8OrU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw8SubU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw8SubU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw8XchgU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw8XchgU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmw8XorU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmw8XorU(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmwAdd operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmwAdd(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmwAnd operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmwAnd(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmwCmpxchg operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmwCmpxchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmwOr operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmwOr(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmwSub operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmwSub(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmwXchg operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmwXchg(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicRmwXor operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicRmwXor(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicStore operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicStore(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicStore16 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicStore16(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicStore32 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicStore32(MemArg arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64AtomicStore8 operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64AtomicStore8(MemArg arg) override; //!< Validate the stack effect of this opcode.


	//! SIMD bitwise, const, bitmask and splat typing.
/**
 * \brief Validates the V128Const instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 */
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
/**
 * \brief Performs the dispatchI16x8Bitmask operation.
 */
	void dispatchI16x8Bitmask() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4Bitmask operation.
 */
	void dispatchI32x4Bitmask() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2Bitmask operation.
 */
	void dispatchI64x2Bitmask() override; //!< Validate the stack effect of this opcode.

/**
 * \brief Performs the dispatchI8x16Swizzle operation.
 */
	void dispatchI8x16Swizzle() override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16RelaxedSwizzle operation.
 */
	void dispatchI8x16RelaxedSwizzle() override; //!< Validate the stack effect of this opcode.

	//! SIMD memory accesses and lanes.
/**
 * \brief Validates the V128Load instruction's immediates and operand-stack effect.
 * \param arg Operand or context value used by this validator operation.
 */
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
/**
 * \brief Performs the dispatchV128Store16Lane operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchV128Store16Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchV128Store32Lane operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchV128Store32Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchV128Store64Lane operation.
 * \param arg1 Input value or output destination for this operation.
 * \param arg2 Input value or output destination for this operation.
 */
	void dispatchV128Store64Lane(MemArg arg1, uint8_t arg2) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16Shuffle operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16ExtractLaneS operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI8x16ExtractLaneS(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16ExtractLaneU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI8x16ExtractLaneU(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI8x16ReplaceLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI8x16ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtractLaneS operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI16x8ExtractLaneS(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ExtractLaneU operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI16x8ExtractLaneU(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI16x8ReplaceLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI16x8ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ExtractLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32x4ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI32x4ReplaceLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI32x4ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ExtractLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64x2ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchI64x2ReplaceLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchI64x2ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4ExtractLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchF32x4ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF32x4ReplaceLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchF32x4ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2ExtractLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchF64x2ExtractLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
/**
 * \brief Performs the dispatchF64x2ReplaceLane operation.
 * \param arg Input value or output destination for this operation.
 */
	void dispatchF64x2ReplaceLane(uint8_t arg) override; //!< Validate the stack effect of this opcode.
};

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMVALIDATOR_HPP

