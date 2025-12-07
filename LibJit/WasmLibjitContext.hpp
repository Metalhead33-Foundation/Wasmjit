#ifndef WASMLIBJITCONTEXT_HPP
#define WASMLIBJITCONTEXT_HPP
#include "../WasmJitContext.hpp"
#include <jit/jit.h>
#include <vector>
#include <map>
#include <stack>

class WasmLibjitContext : public WasmJitContext
{
private:
	struct V128_holder {
		uint64_t low;
		uint64_t high;
	};
public:
	WasmLibjitContext();
	jit_type_t convertBuiltinType(wabt::Type::Enum wabtBuiltinType);
private:
	std::vector<V128_holder> v128_constants;
	std::vector<jit_constant_t> tempConstants;
	std::vector<jit_value_t> jitConstants;
	std::map<size_t,size_t> offsetToConstId;
	std::stack<jit_value_t> stack;
	jit_function_t functionId;
	jit_context_t context;
	jit_type_t v128_type;
	std::byte* linMemAddr;
	// WasmJitContext interface
	size_t createConstant(const wabt::Const& constant);
	jit_value_t createPointer(void* ptr);
protected:
	void handleAtomicFence(const wabt::AtomicFenceExpr& expr) override;
	void handleBlock(const wabt::BlockExpr& expr) override;
	void handleBr(const wabt::BrExpr& expr) override;
	void handleBrIf(const wabt::BrIfExpr& expr) override;
	void handleBrTable(const wabt::BrTableExpr& expr) override;
	void handleCall(const wabt::CallExpr& expr) override;
	void handleCallIndirect(const wabt::CallIndirectExpr& expr) override;
	void handleCallRef(const wabt::CallRefExpr& expr) override;
	void handleCodeMetadata(const wabt::CodeMetadataExpr& expr) override;
	void handleConst(const wabt::ConstExpr& expr) override;
	void handleDrop(const wabt::DropExpr& expr) override;
	void handleGlobalGet(const wabt::GlobalGetExpr& expr) override;
	void handleGlobalSet(const wabt::GlobalSetExpr& expr) override;
	void handleIf(const wabt::IfExpr& expr) override;
	void handleLocalGet(const wabt::LocalGetExpr& expr) override;
	void handleLocalSet(const wabt::LocalSetExpr& expr) override;
	void handleLocalTee(const wabt::LocalTeeExpr& expr) override;
	void handleLoop(const wabt::LoopExpr& expr) override;
	void handleMemoryCopy(const wabt::MemoryCopyExpr& expr) override;
	void handleDataDrop(const wabt::DataDropExpr& expr) override;
	void handleMemoryFill(const wabt::MemoryFillExpr& expr) override;
	void handleMemoryGrow(const wabt::MemoryGrowExpr& expr) override;
	void handleMemoryInit(const wabt::MemoryInitExpr& expr) override;
	void handleMemorySize(const wabt::MemorySizeExpr& expr) override;
	void handleNop(const wabt::NopExpr& expr) override;
	void handleRefIsNull(const wabt::RefIsNullExpr& expr) override;
	void handleRefFunc(const wabt::RefFuncExpr& expr) override;
	void handleRefNull(const wabt::RefNullExpr& expr) override;
	void handleRethrow(const wabt::RethrowExpr& expr) override;
	void handleReturn(const wabt::ReturnExpr& expr) override;
	void handleReturnCall(const wabt::ReturnCallExpr& expr) override;
	void handleReturnCallIndirect(const wabt::ReturnCallIndirectExpr& expr) override;
	void handleSelect(const wabt::SelectExpr& expr) override;
	void handleSimdLaneOp(const wabt::SimdLaneOpExpr& expr) override;
	void handleSimdLoadLane(const wabt::SimdLoadLaneExpr& expr) override;
	void handleSimdStoreLane(const wabt::SimdStoreLaneExpr& expr) override;
	void handleSimdShuffleOp(const wabt::SimdShuffleOpExpr& expr) override;
	void handleTableCopy(const wabt::TableCopyExpr& expr) override;
	void handleElemDrop(const wabt::ElemDropExpr& expr) override;
	void handleTableInit(const wabt::TableInitExpr& expr) override;
	void handleTableGet(const wabt::TableGetExpr& expr) override;
	void handleTableGrow(const wabt::TableGrowExpr& expr) override;
	void handleTableSize(const wabt::TableSizeExpr& expr) override;
	void handleTableSet(const wabt::TableSetExpr& expr) override;
	void handleTableFill(const wabt::TableFillExpr& expr) override;
	void handleThrow(const wabt::ThrowExpr& expr) override;
	void handleThrowRef(const wabt::ThrowRefExpr& expr) override;
	void handleTry(const wabt::TryExpr& expr) override;
	void handleTryTable(const wabt::TryTableExpr& expr) override;
	void handleUnreachable(const wabt::UnreachableExpr& expr) override;
	CompilationType getContextType() const override;
	void betweenStages() override;

	// WasmJitContext interface
protected:
	void handleOperator(const wabt::Location& loc, const wabt::Opcode& opcode, OperatorType operatorType) override;
	void handleLoadStore(const wabt::Location& loc, const wabt::Var& memidx, const wabt::Opcode& opcode, const wabt::Address& align, const wabt::Address& offset, LoadStoreType operatorType) override;
};

#endif // WASMLIBJITCONTEXT_HPP
