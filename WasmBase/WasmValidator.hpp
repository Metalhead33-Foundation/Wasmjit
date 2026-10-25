#ifndef WASMVALIDATOR_HPP
#define WASMVALIDATOR_HPP
#include "WasmModule.hpp"
#include "WasmValidation.hpp"
#include "WasmOpcodeDispatcher.hpp"
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace WASM {

// Structural validator for a decoded module.
//
// Scope (first milestone): every rule that does not need the operand type
// stack — section/index consistency, limits, exports, start, tags, global
// initialisers, active segment targets, and the *immediate* checks inside
// function bodies (memory/table/global/local/function/type/tag indices,
// alignment, lane indices). A body containing an opcode this validator does not
// model yet is conservatively accepted: the strictness lost there is measured
// by `WASM_SPEC_VALIDATE=1`, and the engine must never reject a valid module.
//
// The dispatcher supplies the immediate decoding, so a new opcode needs no
// validator-side byte parsing — only its rule. `onUnhandledOpcode` is how the
// base class reports an opcode the backend/validator does not implement.
class Validator final : public OpcodeDispatcher
{
public:
	explicit Validator(const Module& module, ValidationOptions options = {});

	// Throws std::runtime_error naming the first rule that failed.
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

	// Current function body (npos while validating module-level structure).
	static constexpr std::size_t kNoBody = static_cast<std::size_t>(-1);
	std::size_t currentBody = kNoBody;
	std::vector<ValueType> locals;

	[[noreturn]] void fail(const std::string& message) const;

	// Diagnostic form of a local index in the body being validated
	// (`"0 ($name)"`); the name-section outer key is the global function index.
	std::string describeLocal(uint32_t idx) const;

	FuncType funcTypeAt(uint32_t typeIdx) const;
	// Resolve `cont $ft` to the module-local function type index it names. The
	// canonical view stores a process-wide TypeId, so the module-local index is
	// recovered from `typeIds` (STACK_SWITCHING.md SS-1).
	uint32_t continuationFuncIndex(TypeIdx contTypeIdx, const char* who) const;
	FuncType continuationFuncType(TypeIdx contTypeIdx, const char* who) const;
	// The stage-2 provider carries at most one i32 parameter and one i32 result
	// for a continuation function, a control tag and a handler payload.
	void requireMvpContinuationSignature(const FuncType& type, const char* who) const;
	void requireFunctionIndex(uint32_t idx) const;
	void requireTableIndex(uint32_t idx) const;
	void requireMemoryIndex(uint32_t idx) const;
	// `string.*_array` type checks: the type index must be an array of the
	// required packed element type; encode variants require a mutable array.
	void requireStringArrayType(TypeIdx idx, ValueTypeCode packed, bool requireMutable) const;
	void requireGlobalIndex(uint32_t idx) const;
	void requireTagIndex(uint32_t idx) const;
	void requireLocalIndex(uint32_t idx) const;
	void requireElementIndex(uint32_t idx) const;
	void requireDataIndex(uint32_t idx) const;
	bool globalIsMutable(uint32_t idx) const;
	bool tableIsFuncRef(uint32_t idx) const;
	bool memoryIs64(uint32_t idx) const;
	bool memoryIsShared(uint32_t idx) const;

	// Reference subtyping (nullability + the func/extern/exn/any hierarchies).
	bool refSubtype(const ValueType& actual, const ValueType& expected) const;
	// True when a local of this type has an implicit default value, so reading it
	// before any store is valid. Non-nullable references do not.
	bool isDefaultable(const ValueType& type) const;
	// `ref.func x` may only name a function that the module also mentions
	// outside its function bodies or start function (the spec's `C.refs`).
	std::unordered_set<uint32_t> declaredFunctions;
	std::optional<ValueType> validateConstExpr(const std::vector<uint8_t>& bytes,
			const std::string& label, std::size_t globalLimit);
	bool isEqRef(const ValueType& type) const {
		return refSubtype(type, ValueType{ValueTypeCode::EqRef, -1});
	}
	// A typed reference's concrete heap type must name a declared type.
	void requireWellFormedType(const ValueType& type, const std::string& where) const;

	// `ref.test`/`ref.cast` target construction and the top of the target's
	// heap hierarchy (used for the operand's same-hierarchy rule).
	ValueType refType(bool nullable, const HeapType& heapType) const;
	ValueType refTopForHeap(const HeapType& heapType) const;
	void validateRefTest(bool nullable, const HeapType& heapType);
	void validateRefCast(bool nullable, const HeapType& heapType);

	// Profile gates (docs/proposals/PROFILES.md PR-2/PR-4): reject constructs an
	// excluded marker forbids before any structural rule reports on them.
	void checkProfile() const;
	void checkProfileValueType(const ValueType& type, const std::string& where) const;
	// Prefixed-instruction gates (shared with the base dispatcher's hooks).
	void onSimdOpcode(SIMDOpcode opcode) override;
	void onAtomicOpcode(AtomicOpcode opcode) override;

	void checkLimits(const Limits& limits, bool is64, bool isMemory,
					 const char* what, std::size_t index,
					 uint64_t pageSize = 0x10000) const;
	void requireMemoryAccess(const MemArg& arg, uint32_t naturalLog2) const;
	// Multibyte Array Access (GC_EXTENSIONS.md GX-1): a load/store whose memarg
	// sets bit 5 operates on a GC array. These helpers validate the type index
	// and element shape, enforce mutability for stores, and pop `[ref, address]`
	// instead of the linear-memory address.
	void requireArrayAccess(const MemArg& arg, uint32_t naturalLog2) const;
	void requireArrayMutable(const MemArg& arg) const;
	void popMemoryAddress(const MemArg& arg);
	// Atomic accesses have extra rules: the alignment immediate must equal the
	// natural alignment, and the acquire-release ordering byte is 0 or 1.
	void requireAtomicMemoryAccess(const MemArg& arg, uint32_t naturalLog2) const;
	const CanonFieldType& requireStructField(TypeIdx type, uint32_t fieldIndex) const;
	void requireLane(uint8_t lane, uint32_t laneCount, const char* what) const;

	// Operand/control typing (core instructions only in this milestone).
	struct ControlFrame {
		std::vector<ValueType> startTypes;
		std::vector<ValueType> endTypes;
		std::size_t height = 0;
		bool isLoop = false;
		bool isIf = false;
		bool elseSeen = false;
		bool unreachable = false;
		// Definite assignment of non-defaultable locals. A frame inherits its
		// parent's set on entry; stores made inside the frame do not escape it,
		// and `else` restarts from the entry set (the spec's local-init rule).
		std::vector<uint8_t> initialized;
		std::vector<uint8_t> entryInitialized;
	};
	std::vector<ValueType> typeStack;
	std::vector<ControlFrame> controlStack;
	// Set when an opcode without a typing rule is seen: the rest of the body is
	// still scanned for immediate checks, but the operand stack is not trusted.
	bool typeTainted = false;

	ControlFrame& frame();
	std::vector<ValueType> blockParams(const BlockType& blockType) const;
	std::vector<ValueType> blockResults(const BlockType& blockType) const;
	void pushValue(const ValueType& type);
	std::optional<ValueType> popAny();
	void popExpect(ValueTypeCode code);
	void popExpect(const ValueType& type);
	void pushTypes(const std::vector<ValueType>& types);
	void popTypes(const std::vector<ValueType>& types);
	void checkTailCallResults(const FuncType& callee);
	bool assignable(const ValueType& actual, const ValueType& expected) const;
	// True for `cont`, `nocont` and a concrete `cont $ft` type index. The
	// stack-switching proposal forbids casting to any continuation type.
	bool isContinuationHeap(const HeapType& type) const;
	// More Array Constructors (GC_EXTENSIONS.md GX-2) helpers.
	CoreTypeView requireArrayType(TypeIdx idx, const char* who) const;
	bool storageAssignable(const CanonStorageType& actual,
			const CanonStorageType& expected) const;
	void setUnreachable();
	void beginBlock(const BlockType& blockType, bool isLoop, bool isIf);
	void endBlock(bool isElse);
	void branch(LabelIdx label);
	void branchIf(LabelIdx label);
	ValueTypeCode memoryAddressType(MemIdx memIdx) const;
	ValueTypeCode tableIndexType(TableIdx tableIdx) const;
	FuncType functionType(FuncIdx idx) const;
	ValueType globalType(GlobalIdx idx) const;
	ValueType tableElementType(TableIdx idx) const;
	const FuncType* currentFuncType = nullptr;
	std::vector<ValueType> paramTypes(const FuncType& types) const;
	std::vector<ValueType> resultTypes(const FuncType& types) const;

	// Control and parametric instructions.
	void dispatchUnreachable() override;
	void dispatchNop() override;
	void dispatchBlock(const BlockType& arg) override;
	void dispatchLoop(const BlockType& arg) override;
	void dispatchIf(const BlockType& arg) override;
	void dispatchElse() override;
	void dispatchEnd() override;
	void dispatchBr(LabelIdx arg) override;
	void dispatchBrIf(LabelIdx arg) override;
	void dispatchBrTable(std::vector<LabelIdx>&& arg1, LabelIdx arg2) override;
	void dispatchReturn() override;
	void dispatchDrop() override;
	void dispatchSelect() override;
	void dispatchI32Const(int32_t arg) override;
	void dispatchI64Const(int64_t arg) override;
	void dispatchF32Const(float arg) override;
	void dispatchF64Const(double arg) override;
	void dispatchRefIsNull() override;
	void dispatchRefEq() override;
	void dispatchRefAsNonNull() override;

	// More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2).
	void dispatchArrayNewArray(TypeIdx dst, TypeIdx src) override;
	void dispatchArrayNewMemory(TypeIdx dst, MemIdx src) override;
	void dispatchArrayNewTable(TypeIdx dst, TableIdx src) override;
	// Custom Descriptors (0xFB 32..38).
	void dispatchStructNew(TypeIdx arg) override;
	void dispatchStructNewDefault(TypeIdx arg) override;
	void dispatchStructNewDesc(TypeIdx arg) override;
	void dispatchStructNewDefaultDesc(TypeIdx arg) override;
	void dispatchRefGetDesc(TypeIdx arg) override;
	void dispatchRefCastDescEq(const HeapType& arg) override;
	void dispatchRefCastDescEqNull(const HeapType& arg) override;
	void dispatchBrOnCastDescEq(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override;
	void dispatchBrOnCastDescEqFail(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override;
	void validateCastDescEq(bool nullableTarget, const HeapType& target);
	void validateBrOnCastDescEq(bool isFail, uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2);

	void validateModule();
	void validateGlobalInit(const Global& global, std::size_t index);
	void validateElementSegment(const ElementSegment& segment, std::size_t index);
	void validateDataSegment(const DataSegment& segment, std::size_t index);
	void validateTableInit(const TableType& table, std::size_t index, bool isImport = false);
	void validateFunctionBody(const FunctionBody& body, std::size_t index);

	void dispatchStringNewUtf8(MemIdx arg) override;
	void dispatchStringNewWtf16(MemIdx arg) override;
	void dispatchStringNewLossyUtf8(MemIdx arg) override;
	void dispatchStringNewWtf8(MemIdx arg) override;
	void dispatchStringConst(uint32_t idx) override;
	void dispatchStringMeasureUtf8() override;
	void dispatchStringMeasureWtf8() override;
	void dispatchStringMeasureWtf16() override;
	void dispatchStringEncodeUtf8(MemIdx arg) override;
	void dispatchStringEncodeLossyUtf8(MemIdx arg) override;
	void dispatchStringEncodeWtf8(MemIdx arg) override;
	void dispatchStringEncodeWtf16(MemIdx arg) override;
	void dispatchStringConcat() override;
	void dispatchStringEq() override;
	void dispatchStringIsUsvSequence() override;

	// stringview facility (docs/proposals/STRINGREF.md STR-4)
	void dispatchStringAsWtf8() override;
	void dispatchStringViewWtf8Advance() override;
	void dispatchStringViewWtf8EncodeUtf8(MemIdx arg) override;
	void dispatchStringViewWtf8Slice() override;
	void dispatchStringViewWtf8EncodeLossyUtf8(MemIdx arg) override;
	void dispatchStringViewWtf8EncodeWtf8(MemIdx arg) override;
	void dispatchStringAsWtf16() override;
	void dispatchStringViewWtf16Length() override;
	void dispatchStringViewWtf16GetCodeUnit() override;
	void dispatchStringViewWtf16Encode(MemIdx arg) override;
	void dispatchStringViewWtf16Slice() override;
	void dispatchStringAsIter() override;
	void dispatchStringViewIterNext() override;
	void dispatchStringViewIterAdvance() override;
	void dispatchStringViewIterRewind() override;
	void dispatchStringViewIterSlice() override;

	// GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
	void dispatchStringNewUtf8Array(TypeIdx arg) override;
	void dispatchStringNewWtf16Array(TypeIdx arg) override;
	void dispatchStringEncodeUtf8Array(TypeIdx arg) override;
	void dispatchStringEncodeWtf16Array(TypeIdx arg) override;
	void dispatchStringNewLossyUtf8Array(TypeIdx arg) override;
	void dispatchStringNewWtf8Array(TypeIdx arg) override;
	void dispatchStringEncodeLossyUtf8Array(TypeIdx arg) override;
	void dispatchStringEncodeWtf8Array(TypeIdx arg) override;
	void onUnhandledOpcode(const char* name) override;

	// Immediate-only checks (no operand stack yet).
	void dispatchLocalGet(LocalIdx arg) override;
	void dispatchLocalSet(LocalIdx arg) override;
	void dispatchLocalTee(LocalIdx arg) override;
	void dispatchGlobalGet(GlobalIdx arg) override;
	void dispatchGlobalSet(GlobalIdx arg) override;
	void dispatchCall(FuncIdx arg) override;
	void dispatchReturnCall(FuncIdx arg) override;
	void dispatchCallIndirect(TypeIdx arg1, TableIdx arg2) override;
	void dispatchReturnCallIndirect(TypeIdx arg1, TableIdx arg2) override;
	void dispatchCallRef(TypeIdx arg) override;
	void dispatchReturnCallRef(TypeIdx arg) override;
	void dispatchRefFunc(FuncIdx arg) override;
	void dispatchRefNull(const HeapType& arg) override;
	void dispatchRefTest(const HeapType& arg) override;
	void dispatchRefTestNull(const HeapType& arg) override;
	void dispatchRefCast(const HeapType& arg) override;
	void dispatchRefCastNull(const HeapType& arg) override;
	void dispatchThrow(TagIdx arg) override;
	void dispatchThrowRef() override;
	void dispatchContNew(TypeIdx arg) override;
	void dispatchContBind(TypeIdx arg1, TypeIdx arg2) override;
	void dispatchSuspend(TagIdx arg) override;
	// Shared validation for `resume`/`resume_throw*` handler clauses.
	void validateContHandlers(const std::vector<ContHandlerClause>& handlers);
	void dispatchResume(TypeIdx arg, std::vector<ContHandlerClause>&& handlers) override;
	void dispatchResumeThrow(TypeIdx arg1, TagIdx arg2,
			std::vector<ContHandlerClause>&& handlers) override;
	void dispatchResumeThrowRef(TypeIdx arg,
			std::vector<ContHandlerClause>&& handlers) override;
	void dispatchSwitch(TypeIdx arg1, TagIdx arg2) override;
	void dispatchTryTable(const BlockType& arg1, std::vector<CatchClause>&& arg2) override;
	void dispatchSelectT(std::vector<ValueType>&& arg) override;
	void dispatchBrOnNull(LabelIdx arg) override;
	void dispatchBrOnNonNull(LabelIdx arg) override;
	void dispatchBrOnCast(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override;
	void dispatchBrOnCastFail(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2) override;
	void validateBrOnCast(bool isFail, uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2);

	void dispatchTableGet(TableIdx arg) override;
	void dispatchTableSet(TableIdx arg) override;
	void dispatchTableGrow(TableIdx arg) override;
	void dispatchTableSize(TableIdx arg) override;
	void dispatchTableFill(TableIdx arg) override;
	void dispatchTableCopy(TableIdx arg1, TableIdx arg2) override;
	void dispatchTableInit(uint32_t arg1, TableIdx arg2) override;
	void dispatchElemDrop(uint32_t arg) override;

	void dispatchMemorySize(MemIdx arg) override;
	void dispatchMemoryGrow(MemIdx arg) override;
	void dispatchMemoryInit(uint32_t arg1, MemIdx arg2) override;
	void dispatchDataDrop(uint32_t arg) override;
	void dispatchMemoryCopy(MemIdx arg1, MemIdx arg2) override;
	void dispatchMemoryFill(MemIdx arg) override;
	void dispatchMemoryDiscard(MemIdx arg) override;
	void dispatchRoundingVariant(uint32_t sub) override;
	void dispatchI64Add128() override;
	void dispatchI64Sub128() override;
	void dispatchI64MulWideS() override;
	void dispatchI64MulWideU() override;
	void dispatchI32Itt(uint32_t immediate) override;

	// Core memory accesses.
	void dispatchI32Load(MemArg arg) override;
	void dispatchI64Load(MemArg arg) override;
	void dispatchF32Load(MemArg arg) override;
	void dispatchF64Load(MemArg arg) override;
	void dispatchI32Load8S(MemArg arg) override;
	void dispatchI32Load8U(MemArg arg) override;
	void dispatchI32Load16S(MemArg arg) override;
	void dispatchI32Load16U(MemArg arg) override;
	void dispatchI64Load8S(MemArg arg) override;
	void dispatchI64Load8U(MemArg arg) override;
	void dispatchI64Load16S(MemArg arg) override;
	void dispatchI64Load16U(MemArg arg) override;
	void dispatchI64Load32S(MemArg arg) override;
	void dispatchI64Load32U(MemArg arg) override;
	void dispatchI32Store(MemArg arg) override;
	void dispatchI64Store(MemArg arg) override;
	void dispatchF32Store(MemArg arg) override;
	void dispatchF64Store(MemArg arg) override;
	void dispatchI32Store8(MemArg arg) override;
	void dispatchI32Store16(MemArg arg) override;
	void dispatchI64Store8(MemArg arg) override;
	void dispatchI64Store16(MemArg arg) override;
	void dispatchI64Store32(MemArg arg) override;


	// Core numeric instructions (operand/result typing).
	void dispatchI32Eqz() override;
	void dispatchI64Eqz() override;
	void dispatchF32Abs() override;
	void dispatchF32Ceil() override;
	void dispatchF32Floor() override;
	void dispatchF32Nearest() override;
	void dispatchF32Neg() override;
	void dispatchF32Sqrt() override;
	void dispatchF32Trunc() override;
	void dispatchF64Abs() override;
	void dispatchF64Ceil() override;
	void dispatchF64Floor() override;
	void dispatchF64Nearest() override;
	void dispatchF64Neg() override;
	void dispatchF64Sqrt() override;
	void dispatchF64Trunc() override;
	void dispatchI32Clz() override;
	void dispatchI32Ctz() override;
	void dispatchI32Extend16S() override;
	void dispatchI32Extend8S() override;
	void dispatchI32Popcnt() override;
	void dispatchI64Clz() override;
	void dispatchI64Ctz() override;
	void dispatchI64Extend16S() override;
	void dispatchI64Extend32S() override;
	void dispatchI64Extend8S() override;
	void dispatchI64Popcnt() override;
	void dispatchF32Eq() override;
	void dispatchF32Ge() override;
	void dispatchF32Gt() override;
	void dispatchF32Le() override;
	void dispatchF32Lt() override;
	void dispatchF32Ne() override;
	void dispatchF64Eq() override;
	void dispatchF64Ge() override;
	void dispatchF64Gt() override;
	void dispatchF64Le() override;
	void dispatchF64Lt() override;
	void dispatchF64Ne() override;
	void dispatchI32Eq() override;
	void dispatchI32GeS() override;
	void dispatchI32GeU() override;
	void dispatchI32GtS() override;
	void dispatchI32GtU() override;
	void dispatchI32LeS() override;
	void dispatchI32LeU() override;
	void dispatchI32LtS() override;
	void dispatchI32LtU() override;
	void dispatchI32Ne() override;
	void dispatchI64Eq() override;
	void dispatchI64GeS() override;
	void dispatchI64GeU() override;
	void dispatchI64GtS() override;
	void dispatchI64GtU() override;
	void dispatchI64LeS() override;
	void dispatchI64LeU() override;
	void dispatchI64LtS() override;
	void dispatchI64LtU() override;
	void dispatchI64Ne() override;
	void dispatchF32Add() override;
	void dispatchF32Copysign() override;
	void dispatchF32Div() override;
	void dispatchF32Max() override;
	void dispatchF32Min() override;
	void dispatchF32Mul() override;
	void dispatchF32Sub() override;
	void dispatchF64Add() override;
	void dispatchF64Copysign() override;
	void dispatchF64Div() override;
	void dispatchF64Max() override;
	void dispatchF64Min() override;
	void dispatchF64Mul() override;
	void dispatchF64Sub() override;
	void dispatchI32Add() override;
	void dispatchI32And() override;
	void dispatchI32DivS() override;
	void dispatchI32DivU() override;
	void dispatchI32Mul() override;
	void dispatchI32Or() override;
	void dispatchI32RemS() override;
	void dispatchI32RemU() override;
	void dispatchI32Rotl() override;
	void dispatchI32Rotr() override;
	void dispatchI32Shl() override;
	void dispatchI32ShrS() override;
	void dispatchI32ShrU() override;
	void dispatchI32Sub() override;
	void dispatchI32Xor() override;
	void dispatchI64Add() override;
	void dispatchI64And() override;
	void dispatchI64DivS() override;
	void dispatchI64DivU() override;
	void dispatchI64Mul() override;
	void dispatchI64Or() override;
	void dispatchI64RemS() override;
	void dispatchI64RemU() override;
	void dispatchI64Rotl() override;
	void dispatchI64Rotr() override;
	void dispatchI64Shl() override;
	void dispatchI64ShrS() override;
	void dispatchI64ShrU() override;
	void dispatchI64Sub() override;
	void dispatchI64Xor() override;
	void dispatchF32ConvertI32S() override;
	void dispatchF32ConvertI32U() override;
	void dispatchF32ConvertI64S() override;
	void dispatchF32ConvertI64U() override;
	void dispatchF32DemoteF64() override;
	void dispatchF32ReinterpretI32() override;
	void dispatchF64ConvertI32S() override;
	void dispatchF64ConvertI32U() override;
	void dispatchF64ConvertI64S() override;
	void dispatchF64ConvertI64U() override;
	void dispatchF64PromoteF32() override;
	void dispatchF64ReinterpretI64() override;
	void dispatchI32ReinterpretF32() override;
	void dispatchI32TruncF32S() override;
	void dispatchI32TruncF32U() override;
	void dispatchI32TruncF64S() override;
	void dispatchI32TruncF64U() override;
	void dispatchI32TruncSatF32S() override;
	void dispatchI32TruncSatF32U() override;
	void dispatchI32TruncSatF64S() override;
	void dispatchI32TruncSatF64U() override;
	void dispatchI32WrapI64() override;
	void dispatchI64ExtendI32S() override;
	void dispatchI64ExtendI32U() override;
	void dispatchI64ReinterpretF64() override;
	void dispatchI64TruncF32S() override;
	void dispatchI64TruncF32U() override;
	void dispatchI64TruncF64S() override;
	void dispatchI64TruncF64U() override;
	void dispatchI64TruncSatF32S() override;
	void dispatchI64TruncSatF32U() override;
	void dispatchI64TruncSatF64S() override;
	void dispatchI64TruncSatF64U() override;


	// SIMD arithmetic/comparison/conversion typing (from wasm_simd_ops.inc).
	void dispatchI8x16Add() override;
	void dispatchI8x16Sub() override;
	void dispatchI8x16MinS() override;
	void dispatchI8x16MinU() override;
	void dispatchI8x16MaxS() override;
	void dispatchI8x16MaxU() override;
	void dispatchI16x8Add() override;
	void dispatchI16x8Sub() override;
	void dispatchI16x8Mul() override;
	void dispatchI16x8MinS() override;
	void dispatchI16x8MinU() override;
	void dispatchI16x8MaxS() override;
	void dispatchI16x8MaxU() override;
	void dispatchI32x4Add() override;
	void dispatchI32x4Sub() override;
	void dispatchI32x4Mul() override;
	void dispatchI32x4MinS() override;
	void dispatchI32x4MinU() override;
	void dispatchI32x4MaxS() override;
	void dispatchI32x4MaxU() override;
	void dispatchI64x2Add() override;
	void dispatchI64x2Sub() override;
	void dispatchI64x2Mul() override;
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
	void dispatchI64x2Eq() override;
	void dispatchI64x2Ne() override;
	void dispatchI64x2LtS() override;
	void dispatchI64x2GtS() override;
	void dispatchI64x2LeS() override;
	void dispatchI64x2GeS() override;
	void dispatchI8x16Neg() override;
	void dispatchI16x8Neg() override;
	void dispatchI32x4Neg() override;
	void dispatchI64x2Neg() override;
	void dispatchI8x16Abs() override;
	void dispatchI16x8Abs() override;
	void dispatchI32x4Abs() override;
	void dispatchI64x2Abs() override;
	void dispatchI8x16Shl() override;
	void dispatchI8x16ShrS() override;
	void dispatchI8x16ShrU() override;
	void dispatchI16x8Shl() override;
	void dispatchI16x8ShrS() override;
	void dispatchI16x8ShrU() override;
	void dispatchI32x4Shl() override;
	void dispatchI32x4ShrS() override;
	void dispatchI32x4ShrU() override;
	void dispatchI64x2Shl() override;
	void dispatchI64x2ShrS() override;
	void dispatchI64x2ShrU() override;
	void dispatchI8x16AddSatS() override;
	void dispatchI8x16AddSatU() override;
	void dispatchI8x16SubSatS() override;
	void dispatchI8x16SubSatU() override;
	void dispatchI16x8AddSatS() override;
	void dispatchI16x8AddSatU() override;
	void dispatchI16x8SubSatS() override;
	void dispatchI16x8SubSatU() override;
	void dispatchI8x16AvgrU() override;
	void dispatchI16x8AvgrU() override;
	void dispatchI8x16Popcnt() override;
	void dispatchI16x8Q15MulRSatS() override;
	void dispatchI8x16AllTrue() override;
	void dispatchI16x8AllTrue() override;
	void dispatchI32x4AllTrue() override;
	void dispatchI64x2AllTrue() override;
	void dispatchV128AnyTrue() override;
	void dispatchI16x8ExtendLowI8x16S() override;
	void dispatchI16x8ExtendHighI8x16S() override;
	void dispatchI16x8ExtendLowI8x16U() override;
	void dispatchI16x8ExtendHighI8x16U() override;
	void dispatchI32x4ExtendLowI16x8S() override;
	void dispatchI32x4ExtendHighI16x8S() override;
	void dispatchI32x4ExtendLowI16x8U() override;
	void dispatchI32x4ExtendHighI16x8U() override;
	void dispatchI64x2ExtendLowI32x4S() override;
	void dispatchI64x2ExtendHighI32x4S() override;
	void dispatchI64x2ExtendLowI32x4U() override;
	void dispatchI64x2ExtendHighI32x4U() override;
	void dispatchI16x8ExtMulLowI8x16S() override;
	void dispatchI16x8ExtMulHighI8x16S() override;
	void dispatchI16x8ExtMulLowI8x16U() override;
	void dispatchI16x8ExtMulHighI8x16U() override;
	void dispatchI32x4ExtMulLowI16x8S() override;
	void dispatchI32x4ExtMulHighI16x8S() override;
	void dispatchI32x4ExtMulLowI16x8U() override;
	void dispatchI32x4ExtMulHighI16x8U() override;
	void dispatchI64x2ExtMulLowI32x4S() override;
	void dispatchI64x2ExtMulHighI32x4S() override;
	void dispatchI64x2ExtMulLowI32x4U() override;
	void dispatchI64x2ExtMulHighI32x4U() override;
	void dispatchI16x8ExtAddPairwiseI8x16S() override;
	void dispatchI16x8ExtAddPairwiseI8x16U() override;
	void dispatchI32x4ExtAddPairwiseI16x8S() override;
	void dispatchI32x4ExtAddPairwiseI16x8U() override;
	void dispatchI32x4DotI16x8S() override;
	void dispatchI8x16NarrowI16x8S() override;
	void dispatchI8x16NarrowI16x8U() override;
	void dispatchI16x8NarrowI32x4S() override;
	void dispatchI16x8NarrowI32x4U() override;
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
	void dispatchF32x4Ceil() override;
	void dispatchF32x4Floor() override;
	void dispatchF32x4Trunc() override;
	void dispatchF32x4Nearest() override;
	void dispatchF32x4Abs() override;
	void dispatchF32x4Neg() override;
	void dispatchF32x4Sqrt() override;
	void dispatchF64x2Ceil() override;
	void dispatchF64x2Floor() override;
	void dispatchF64x2Trunc() override;
	void dispatchF64x2Nearest() override;
	void dispatchF64x2Abs() override;
	void dispatchF64x2Neg() override;
	void dispatchF64x2Sqrt() override;
	void dispatchF32x4Add() override;
	void dispatchF32x4Sub() override;
	void dispatchF32x4Mul() override;
	void dispatchF32x4Div() override;
	void dispatchF32x4Min() override;
	void dispatchF32x4Max() override;
	void dispatchF32x4PMin() override;
	void dispatchF32x4PMax() override;
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
	void dispatchF64x2ConvertLowI32x4S() override;
	void dispatchF64x2ConvertLowI32x4U() override;
	void dispatchF64x2PromoteLowF32x4() override;
	void dispatchI32x4TruncSatF64x2SZero() override;
	void dispatchI32x4TruncSatF64x2UZero() override;
	void dispatchF32x4DemoteF64x2Zero() override;
	void dispatchI32x4RelaxedTruncF32x4S() override;
	void dispatchI32x4RelaxedTruncF32x4U() override;
	void dispatchI32x4RelaxedTruncF64x2SZero() override;
	void dispatchI32x4RelaxedTruncF64x2UZero() override;
	void dispatchF32x4RelaxedMin() override;
	void dispatchF32x4RelaxedMax() override;
	void dispatchF64x2RelaxedMin() override;
	void dispatchF64x2RelaxedMax() override;
	void dispatchI16x8RelaxedQ15MulRS() override;
	void dispatchF32x4RelaxedMadd() override;
	void dispatchF32x4RelaxedNmadd() override;
	void dispatchF64x2RelaxedMadd() override;
	void dispatchF64x2RelaxedNmadd() override;
	void dispatchI8x16RelaxedLaneselect() override;
	void dispatchI16x8RelaxedLaneselect() override;
	void dispatchI32x4RelaxedLaneselect() override;
	void dispatchI64x2RelaxedLaneselect() override;
	void dispatchI16x8RelaxedDotI8x16I7x16S() override;
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override;


	// Atomic memory accesses (operand/result typing).
	void dispatchMemoryAtomicNotify(MemArg arg) override;
	void dispatchMemoryAtomicWait32(MemArg arg) override;
	void dispatchMemoryAtomicWait64(MemArg arg) override;
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
	void dispatchI32AtomicLoad(MemArg arg) override;
	void dispatchI32AtomicLoad16U(MemArg arg) override;
	void dispatchI32AtomicLoad8U(MemArg arg) override;
	void dispatchI32AtomicRmw16AddU(MemArg arg) override;
	void dispatchI32AtomicRmw16AndU(MemArg arg) override;
	void dispatchI32AtomicRmw16CmpxchgU(MemArg arg) override;
	void dispatchI32AtomicRmw16OrU(MemArg arg) override;
	void dispatchI32AtomicRmw16SubU(MemArg arg) override;
	void dispatchI32AtomicRmw16XchgU(MemArg arg) override;
	void dispatchI32AtomicRmw16XorU(MemArg arg) override;
	void dispatchI32AtomicRmw8AddU(MemArg arg) override;
	void dispatchI32AtomicRmw8AndU(MemArg arg) override;
	void dispatchI32AtomicRmw8CmpxchgU(MemArg arg) override;
	void dispatchI32AtomicRmw8OrU(MemArg arg) override;
	void dispatchI32AtomicRmw8SubU(MemArg arg) override;
	void dispatchI32AtomicRmw8XchgU(MemArg arg) override;
	void dispatchI32AtomicRmw8XorU(MemArg arg) override;
	void dispatchI32AtomicRmwAdd(MemArg arg) override;
	void dispatchI32AtomicRmwAnd(MemArg arg) override;
	void dispatchI32AtomicRmwCmpxchg(MemArg arg) override;
	void dispatchI32AtomicRmwOr(MemArg arg) override;
	void dispatchI32AtomicRmwSub(MemArg arg) override;
	void dispatchI32AtomicRmwXchg(MemArg arg) override;
	void dispatchI32AtomicRmwXor(MemArg arg) override;
	void dispatchI32AtomicStore(MemArg arg) override;
	void dispatchI32AtomicStore16(MemArg arg) override;
	void dispatchI32AtomicStore8(MemArg arg) override;
	void dispatchI64AtomicLoad(MemArg arg) override;
	void dispatchI64AtomicLoad16U(MemArg arg) override;
	void dispatchI64AtomicLoad32U(MemArg arg) override;
	void dispatchI64AtomicLoad8U(MemArg arg) override;
	void dispatchI64AtomicRmw16AddU(MemArg arg) override;
	void dispatchI64AtomicRmw16AndU(MemArg arg) override;
	void dispatchI64AtomicRmw16CmpxchgU(MemArg arg) override;
	void dispatchI64AtomicRmw16OrU(MemArg arg) override;
	void dispatchI64AtomicRmw16SubU(MemArg arg) override;
	void dispatchI64AtomicRmw16XchgU(MemArg arg) override;
	void dispatchI64AtomicRmw16XorU(MemArg arg) override;
	void dispatchI64AtomicRmw32AddU(MemArg arg) override;
	void dispatchI64AtomicRmw32AndU(MemArg arg) override;
	void dispatchI64AtomicRmw32CmpxchgU(MemArg arg) override;
	void dispatchI64AtomicRmw32OrU(MemArg arg) override;
	void dispatchI64AtomicRmw32SubU(MemArg arg) override;
	void dispatchI64AtomicRmw32XchgU(MemArg arg) override;
	void dispatchI64AtomicRmw32XorU(MemArg arg) override;
	void dispatchI64AtomicRmw8AddU(MemArg arg) override;
	void dispatchI64AtomicRmw8AndU(MemArg arg) override;
	void dispatchI64AtomicRmw8CmpxchgU(MemArg arg) override;
	void dispatchI64AtomicRmw8OrU(MemArg arg) override;
	void dispatchI64AtomicRmw8SubU(MemArg arg) override;
	void dispatchI64AtomicRmw8XchgU(MemArg arg) override;
	void dispatchI64AtomicRmw8XorU(MemArg arg) override;
	void dispatchI64AtomicRmwAdd(MemArg arg) override;
	void dispatchI64AtomicRmwAnd(MemArg arg) override;
	void dispatchI64AtomicRmwCmpxchg(MemArg arg) override;
	void dispatchI64AtomicRmwOr(MemArg arg) override;
	void dispatchI64AtomicRmwSub(MemArg arg) override;
	void dispatchI64AtomicRmwXchg(MemArg arg) override;
	void dispatchI64AtomicRmwXor(MemArg arg) override;
	void dispatchI64AtomicStore(MemArg arg) override;
	void dispatchI64AtomicStore16(MemArg arg) override;
	void dispatchI64AtomicStore32(MemArg arg) override;
	void dispatchI64AtomicStore8(MemArg arg) override;


	// SIMD bitwise, const, bitmask and splat typing.
	void dispatchV128Const(std::span<uint8_t> arg) override;
	void dispatchV128Not() override;
	void dispatchV128And() override;
	void dispatchV128AndNot() override;
	void dispatchV128Or() override;
	void dispatchV128Xor() override;
	void dispatchV128Bitselect() override;
	void dispatchI8x16Splat() override;
	void dispatchI16x8Splat() override;
	void dispatchI32x4Splat() override;
	void dispatchI64x2Splat() override;
	void dispatchF32x4Splat() override;
	void dispatchF64x2Splat() override;

	// Bitmask reductions are part of the wasm_simd_ops.inc shape list.
	void dispatchI8x16Bitmask() override;
	void dispatchI16x8Bitmask() override;
	void dispatchI32x4Bitmask() override;
	void dispatchI64x2Bitmask() override;

	void dispatchI8x16Swizzle() override;
	void dispatchI8x16RelaxedSwizzle() override;

	// SIMD memory accesses and lanes.
	void dispatchV128Load(MemArg arg) override;
	void dispatchV128Load8x8S(MemArg arg) override;
	void dispatchV128Load8x8U(MemArg arg) override;
	void dispatchV128Load16x4S(MemArg arg) override;
	void dispatchV128Load16x4U(MemArg arg) override;
	void dispatchV128Load32x2S(MemArg arg) override;
	void dispatchV128Load32x2U(MemArg arg) override;
	void dispatchV128Load8Splat(MemArg arg) override;
	void dispatchV128Load16Splat(MemArg arg) override;
	void dispatchV128Load32Splat(MemArg arg) override;
	void dispatchV128Load64Splat(MemArg arg) override;
	void dispatchV128Load32Zero(MemArg arg) override;
	void dispatchV128Load64Zero(MemArg arg) override;
	void dispatchV128Store(MemArg arg) override;
	void dispatchV128Load8Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchV128Load16Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchV128Load32Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchV128Load64Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store8Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store16Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store32Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchV128Store64Lane(MemArg arg1, uint8_t arg2) override;
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override;
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
};

} // namespace WASM

#endif // WASMVALIDATOR_HPP

