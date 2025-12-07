#ifndef WASMJITCONTEXT_HPP
#define WASMJITCONTEXT_HPP
#include <cstdint>
#include <wabt/ir.h>

class WasmJitContext {
public:
	enum CompilationType : uint8_t {
		ALL_IN_ONE,
		TWO_STAGE_CONST_FIRST
	};
	enum OperatorType : uint8_t {
		Binary,
		Compare,
		Convert,
		Unary,
		Ternary
	};
	enum LoadStoreType : uint8_t {
		Load,
		Store,
		AtomicLoad,
		AtomicStore,
		AtomicRmw,
		AtomicRmwCmpxchg,
		AtomicWait,
		AtomicNotify,
		LoadSplat,
		LoadZero
	};
protected:
	virtual void handleOperator(const wabt::Location& loc, const wabt::Opcode& opcode, OperatorType operatorType) = 0;
	virtual void handleLoadStore(const wabt::Location& loc, const wabt::Var& memidx, const wabt::Opcode& opcode,
								 const wabt::Address& align, const wabt::Address& offset, LoadStoreType operatorType) = 0;
	virtual void handleAtomicFence(const wabt::AtomicFenceExpr& expr) = 0;
	virtual void handleBlock(const wabt::BlockExpr& expr) = 0;
	virtual void handleBr(const wabt::BrExpr& expr) = 0;
	virtual void handleBrIf(const wabt::BrIfExpr& expr) = 0;
	virtual void handleBrTable(const wabt::BrTableExpr& expr) = 0;
	virtual void handleCall(const wabt::CallExpr& expr) = 0;
	virtual void handleCallIndirect(const wabt::CallIndirectExpr& expr) = 0;
	virtual void handleCallRef(const wabt::CallRefExpr& expr) = 0;
	virtual void handleCodeMetadata(const wabt::CodeMetadataExpr& expr) = 0;
	virtual void handleConst(const wabt::ConstExpr& expr) = 0;
	virtual void handleDrop(const wabt::DropExpr& expr) = 0;
	virtual void handleGlobalGet(const wabt::GlobalGetExpr& expr) = 0;
	virtual void handleGlobalSet(const wabt::GlobalSetExpr& expr) = 0;
	virtual void handleIf(const wabt::IfExpr& expr) = 0;
	virtual void handleLocalGet(const wabt::LocalGetExpr& expr) = 0;
	virtual void handleLocalSet(const wabt::LocalSetExpr& expr) = 0;
	virtual void handleLocalTee(const wabt::LocalTeeExpr& expr) = 0;
	virtual void handleLoop(const wabt::LoopExpr& expr) = 0;
	virtual void handleMemoryCopy(const wabt::MemoryCopyExpr& expr) = 0;
	virtual void handleDataDrop(const wabt::DataDropExpr& expr) = 0;
	virtual void handleMemoryFill(const wabt::MemoryFillExpr& expr) = 0;
	virtual void handleMemoryGrow(const wabt::MemoryGrowExpr& expr) = 0;
	virtual void handleMemoryInit(const wabt::MemoryInitExpr& expr) = 0;
	virtual void handleMemorySize(const wabt::MemorySizeExpr& expr) = 0;
	virtual void handleNop(const wabt::NopExpr& expr) = 0;
	virtual void handleRefIsNull(const wabt::RefIsNullExpr& expr) = 0;
	virtual void handleRefFunc(const wabt::RefFuncExpr& expr) = 0;
	virtual void handleRefNull(const wabt::RefNullExpr& expr) = 0;
	virtual void handleRethrow(const wabt::RethrowExpr& expr) = 0;
	virtual void handleReturn(const wabt::ReturnExpr& expr) = 0;
	virtual void handleReturnCall(const wabt::ReturnCallExpr& expr) = 0;
	virtual void handleReturnCallIndirect(const wabt::ReturnCallIndirectExpr& expr) = 0;
	virtual void handleSelect(const wabt::SelectExpr& expr) = 0;
	virtual void handleSimdLaneOp(const wabt::SimdLaneOpExpr& expr) = 0;
	virtual void handleSimdLoadLane(const wabt::SimdLoadLaneExpr& expr) = 0;
	virtual void handleSimdStoreLane(const wabt::SimdStoreLaneExpr& expr) = 0;
	virtual void handleSimdShuffleOp(const wabt::SimdShuffleOpExpr& expr) = 0;
	virtual void handleTableCopy(const wabt::TableCopyExpr& expr) = 0;
	virtual void handleElemDrop(const wabt::ElemDropExpr& expr) = 0;
	virtual void handleTableInit(const wabt::TableInitExpr& expr) = 0;
	virtual void handleTableGet(const wabt::TableGetExpr& expr) = 0;
	virtual void handleTableGrow(const wabt::TableGrowExpr& expr) = 0;
	virtual void handleTableSize(const wabt::TableSizeExpr& expr) = 0;
	virtual void handleTableSet(const wabt::TableSetExpr& expr) = 0;
	virtual void handleTableFill(const wabt::TableFillExpr& expr) = 0;
	virtual void handleThrow(const wabt::ThrowExpr& expr) = 0;
	virtual void handleThrowRef(const wabt::ThrowRefExpr& expr) = 0;
	virtual void handleTry(const wabt::TryExpr& expr) = 0;
	virtual void handleTryTable(const wabt::TryTableExpr& expr) = 0;
	virtual void handleUnreachable(const wabt::UnreachableExpr& expr) = 0;
	virtual CompilationType getContextType() const = 0;
	virtual void betweenStages() {};
public:
	virtual ~WasmJitContext() = default;
	inline void emitExprs(const wabt::ExprList& exprs)
	{
		CompilationType contextType = getContextType();
		switch (contextType) {
			case ALL_IN_ONE:
			{
				for (const auto& expr : exprs) {
					switch (expr.type()) {
						case wabt::ExprType::AtomicLoad: { const auto& converted_expr = dynamic_cast<const wabt::AtomicLoadExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicLoad ); break; }
						case wabt::ExprType::AtomicRmw: { const auto& converted_expr = dynamic_cast<const wabt::AtomicRmwExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicRmw ); break; }
						case wabt::ExprType::AtomicRmwCmpxchg: { const auto& converted_expr = dynamic_cast<const wabt::AtomicRmwCmpxchgExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicRmwCmpxchg ); break; }
						case wabt::ExprType::AtomicStore: { const auto& converted_expr = dynamic_cast<const wabt::AtomicStoreExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicStore ); break; }
						case wabt::ExprType::AtomicNotify: { const auto& converted_expr = dynamic_cast<const wabt::AtomicNotifyExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicNotify ); break; }
						case wabt::ExprType::AtomicFence: { const auto& converted_expr = dynamic_cast<const wabt::AtomicFenceExpr&>(expr); handleAtomicFence(converted_expr); break; }
						case wabt::ExprType::AtomicWait: { const auto& converted_expr = dynamic_cast<const wabt::AtomicWaitExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicWait ); break; }
						case wabt::ExprType::Binary: { const auto& converted_expr = dynamic_cast<const wabt::BinaryExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Binary ); break; }
						case wabt::ExprType::Block: { const auto& converted_expr = dynamic_cast<const wabt::BlockExpr&>(expr); handleBlock(converted_expr); break; }
						case wabt::ExprType::Br: { const auto& converted_expr = dynamic_cast<const wabt::BrExpr&>(expr); handleBr(converted_expr); break; }
						case wabt::ExprType::BrIf: { const auto& converted_expr = dynamic_cast<const wabt::BrIfExpr&>(expr); handleBrIf(converted_expr); break; }
						case wabt::ExprType::BrTable: { const auto& converted_expr = dynamic_cast<const wabt::BrTableExpr&>(expr); handleBrTable(converted_expr); break; }
						case wabt::ExprType::Call: { const auto& converted_expr = dynamic_cast<const wabt::CallExpr&>(expr); handleCall(converted_expr); break; }
						case wabt::ExprType::CallIndirect: { const auto& converted_expr = dynamic_cast<const wabt::CallIndirectExpr&>(expr); handleCallIndirect(converted_expr); break; }
						case wabt::ExprType::CallRef: { const auto& converted_expr = dynamic_cast<const wabt::CallRefExpr&>(expr); handleCallRef(converted_expr); break; }
						case wabt::ExprType::CodeMetadata: { const auto& converted_expr = dynamic_cast<const wabt::CodeMetadataExpr&>(expr); handleCodeMetadata(converted_expr); break; }
						case wabt::ExprType::Compare: { const auto& converted_expr = dynamic_cast<const wabt::CompareExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Compare ); break; }
						case wabt::ExprType::Const: { const auto& converted_expr = dynamic_cast<const wabt::ConstExpr&>(expr); handleConst(converted_expr); break; }
						case wabt::ExprType::Convert: { const auto& converted_expr = dynamic_cast<const wabt::ConvertExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Convert ); break; }
						case wabt::ExprType::Drop: { const auto& converted_expr = dynamic_cast<const wabt::DropExpr&>(expr); handleDrop(converted_expr); break; }
						case wabt::ExprType::GlobalGet: { const auto& converted_expr = dynamic_cast<const wabt::GlobalGetExpr&>(expr); handleGlobalGet(converted_expr); break; }
						case wabt::ExprType::GlobalSet: { const auto& converted_expr = dynamic_cast<const wabt::GlobalSetExpr&>(expr); handleGlobalSet(converted_expr); break; }
						case wabt::ExprType::If: { const auto& converted_expr = dynamic_cast<const wabt::IfExpr&>(expr); handleIf(converted_expr); break; }
						case wabt::ExprType::Load: { const auto& converted_expr = dynamic_cast<const wabt::LoadExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::Load ); break; }
						case wabt::ExprType::LocalGet: { const auto& converted_expr = dynamic_cast<const wabt::LocalGetExpr&>(expr); handleLocalGet(converted_expr); break; }
						case wabt::ExprType::LocalSet: { const auto& converted_expr = dynamic_cast<const wabt::LocalSetExpr&>(expr); handleLocalSet(converted_expr); break; }
						case wabt::ExprType::LocalTee: { const auto& converted_expr = dynamic_cast<const wabt::LocalTeeExpr&>(expr); handleLocalTee(converted_expr); break; }
						case wabt::ExprType::Loop: { const auto& converted_expr = dynamic_cast<const wabt::LoopExpr&>(expr); handleLoop(converted_expr); break; }
						case wabt::ExprType::MemoryCopy: { const auto& converted_expr = dynamic_cast<const wabt::MemoryCopyExpr&>(expr); handleMemoryCopy(converted_expr); break; }
						case wabt::ExprType::DataDrop: { const auto& converted_expr = dynamic_cast<const wabt::DataDropExpr&>(expr); handleDataDrop(converted_expr); break; }
						case wabt::ExprType::MemoryFill: { const auto& converted_expr = dynamic_cast<const wabt::MemoryFillExpr&>(expr); handleMemoryFill(converted_expr); break; }
						case wabt::ExprType::MemoryGrow: { const auto& converted_expr = dynamic_cast<const wabt::MemoryGrowExpr&>(expr); handleMemoryGrow(converted_expr); break; }
						case wabt::ExprType::MemoryInit: { const auto& converted_expr = dynamic_cast<const wabt::MemoryInitExpr&>(expr); handleMemoryInit(converted_expr); break; }
						case wabt::ExprType::MemorySize: { const auto& converted_expr = dynamic_cast<const wabt::MemorySizeExpr&>(expr); handleMemorySize(converted_expr); break; }
						case wabt::ExprType::Nop: { const auto& converted_expr = dynamic_cast<const wabt::NopExpr&>(expr); handleNop(converted_expr); break; }
						case wabt::ExprType::RefIsNull: { const auto& converted_expr = dynamic_cast<const wabt::RefIsNullExpr&>(expr); handleRefIsNull(converted_expr); break; }
						case wabt::ExprType::RefFunc: { const auto& converted_expr = dynamic_cast<const wabt::RefFuncExpr&>(expr); handleRefFunc(converted_expr); break; }
						case wabt::ExprType::RefNull: { const auto& converted_expr = dynamic_cast<const wabt::RefNullExpr&>(expr); handleRefNull(converted_expr); break; }
						case wabt::ExprType::Rethrow: { const auto& converted_expr = dynamic_cast<const wabt::RethrowExpr&>(expr); handleRethrow(converted_expr); break; }
						case wabt::ExprType::Return: { const auto& converted_expr = dynamic_cast<const wabt::ReturnExpr&>(expr); handleReturn(converted_expr); break; }
						case wabt::ExprType::ReturnCall: { const auto& converted_expr = dynamic_cast<const wabt::ReturnCallExpr&>(expr); handleReturnCall(converted_expr); break; }
						case wabt::ExprType::ReturnCallIndirect: { const auto& converted_expr = dynamic_cast<const wabt::ReturnCallIndirectExpr&>(expr); handleReturnCallIndirect(converted_expr); break; }
						case wabt::ExprType::Select: { const auto& converted_expr = dynamic_cast<const wabt::SelectExpr&>(expr); handleSelect(converted_expr); break; }
						case wabt::ExprType::SimdLaneOp: { const auto& converted_expr = dynamic_cast<const wabt::SimdLaneOpExpr&>(expr); handleSimdLaneOp(converted_expr); break; }
						case wabt::ExprType::SimdLoadLane: { const auto& converted_expr = dynamic_cast<const wabt::SimdLoadLaneExpr&>(expr); handleSimdLoadLane(converted_expr); break; }
						case wabt::ExprType::SimdStoreLane: { const auto& converted_expr = dynamic_cast<const wabt::SimdStoreLaneExpr&>(expr); handleSimdStoreLane(converted_expr); break; }
						case wabt::ExprType::SimdShuffleOp: { const auto& converted_expr = dynamic_cast<const wabt::SimdShuffleOpExpr&>(expr); handleSimdShuffleOp(converted_expr); break; }
						case wabt::ExprType::LoadSplat: { const auto& converted_expr = dynamic_cast<const wabt::LoadSplatExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::LoadSplat ); break; }
						case wabt::ExprType::LoadZero: { const auto& converted_expr = dynamic_cast<const wabt::LoadZeroExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::LoadZero ); break; }
						case wabt::ExprType::Store: { const auto& converted_expr = dynamic_cast<const wabt::StoreExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::Store ); break; }
						case wabt::ExprType::TableCopy: { const auto& converted_expr = dynamic_cast<const wabt::TableCopyExpr&>(expr); handleTableCopy(converted_expr); break; }
						case wabt::ExprType::ElemDrop: { const auto& converted_expr = dynamic_cast<const wabt::ElemDropExpr&>(expr); handleElemDrop(converted_expr); break; }
						case wabt::ExprType::TableInit: { const auto& converted_expr = dynamic_cast<const wabt::TableInitExpr&>(expr); handleTableInit(converted_expr); break; }
						case wabt::ExprType::TableGet: { const auto& converted_expr = dynamic_cast<const wabt::TableGetExpr&>(expr); handleTableGet(converted_expr); break; }
						case wabt::ExprType::TableGrow: { const auto& converted_expr = dynamic_cast<const wabt::TableGrowExpr&>(expr); handleTableGrow(converted_expr); break; }
						case wabt::ExprType::TableSize: { const auto& converted_expr = dynamic_cast<const wabt::TableSizeExpr&>(expr); handleTableSize(converted_expr); break; }
						case wabt::ExprType::TableSet: { const auto& converted_expr = dynamic_cast<const wabt::TableSetExpr&>(expr); handleTableSet(converted_expr); break; }
						case wabt::ExprType::TableFill: { const auto& converted_expr = dynamic_cast<const wabt::TableFillExpr&>(expr); handleTableFill(converted_expr); break; }
						case wabt::ExprType::Ternary: { const auto& converted_expr = dynamic_cast<const wabt::TernaryExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Ternary ); break; }
						case wabt::ExprType::Throw: { const auto& converted_expr = dynamic_cast<const wabt::ThrowExpr&>(expr); handleThrow(converted_expr); break; }
						case wabt::ExprType::ThrowRef: { const auto& converted_expr = dynamic_cast<const wabt::ThrowRefExpr&>(expr); handleThrowRef(converted_expr); break; }
						case wabt::ExprType::Try: { const auto& converted_expr = dynamic_cast<const wabt::TryExpr&>(expr); handleTry(converted_expr); break; }
						case wabt::ExprType::TryTable: { const auto& converted_expr = dynamic_cast<const wabt::TryTableExpr&>(expr); handleTryTable(converted_expr); break; }
						case wabt::ExprType::Unary: { const auto& converted_expr = dynamic_cast<const wabt::UnaryExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Unary ); break; }
						case wabt::ExprType::Unreachable: { const auto& converted_expr = dynamic_cast<const wabt::UnreachableExpr&>(expr); handleUnreachable(converted_expr); break; }
						default: break;
					}
				}
				break;
			}
			case TWO_STAGE_CONST_FIRST: {
				for (const auto& expr : exprs) {
					switch (expr.type()) {
						case wabt::ExprType::Const: { const auto& converted_expr = dynamic_cast<const wabt::ConstExpr&>(expr); handleConst(converted_expr); break; }
						default: break;
					}
				}
				betweenStages();
				for (const auto& expr : exprs) {
					switch (expr.type()) {
						case wabt::ExprType::AtomicLoad: { const auto& converted_expr = dynamic_cast<const wabt::AtomicLoadExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicLoad ); break; }
						case wabt::ExprType::AtomicRmw: { const auto& converted_expr = dynamic_cast<const wabt::AtomicRmwExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicRmw ); break; }
						case wabt::ExprType::AtomicRmwCmpxchg: { const auto& converted_expr = dynamic_cast<const wabt::AtomicRmwCmpxchgExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicRmwCmpxchg ); break; }
						case wabt::ExprType::AtomicStore: { const auto& converted_expr = dynamic_cast<const wabt::AtomicStoreExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicStore ); break; }
						case wabt::ExprType::AtomicNotify: { const auto& converted_expr = dynamic_cast<const wabt::AtomicNotifyExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicNotify ); break; }
						case wabt::ExprType::AtomicFence: { const auto& converted_expr = dynamic_cast<const wabt::AtomicFenceExpr&>(expr); handleAtomicFence(converted_expr); break; }
						case wabt::ExprType::AtomicWait: { const auto& converted_expr = dynamic_cast<const wabt::AtomicWaitExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::AtomicWait ); break; }
						case wabt::ExprType::Binary: { const auto& converted_expr = dynamic_cast<const wabt::BinaryExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Binary ); break; }
						case wabt::ExprType::Block: { const auto& converted_expr = dynamic_cast<const wabt::BlockExpr&>(expr); handleBlock(converted_expr); break; }
						case wabt::ExprType::Br: { const auto& converted_expr = dynamic_cast<const wabt::BrExpr&>(expr); handleBr(converted_expr); break; }
						case wabt::ExprType::BrIf: { const auto& converted_expr = dynamic_cast<const wabt::BrIfExpr&>(expr); handleBrIf(converted_expr); break; }
						case wabt::ExprType::BrTable: { const auto& converted_expr = dynamic_cast<const wabt::BrTableExpr&>(expr); handleBrTable(converted_expr); break; }
						case wabt::ExprType::Call: { const auto& converted_expr = dynamic_cast<const wabt::CallExpr&>(expr); handleCall(converted_expr); break; }
						case wabt::ExprType::CallIndirect: { const auto& converted_expr = dynamic_cast<const wabt::CallIndirectExpr&>(expr); handleCallIndirect(converted_expr); break; }
						case wabt::ExprType::CallRef: { const auto& converted_expr = dynamic_cast<const wabt::CallRefExpr&>(expr); handleCallRef(converted_expr); break; }
						case wabt::ExprType::CodeMetadata: { const auto& converted_expr = dynamic_cast<const wabt::CodeMetadataExpr&>(expr); handleCodeMetadata(converted_expr); break; }
						case wabt::ExprType::Compare: { const auto& converted_expr = dynamic_cast<const wabt::CompareExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Compare ); break; }
						case wabt::ExprType::Const: { break; }
						case wabt::ExprType::Convert: { const auto& converted_expr = dynamic_cast<const wabt::ConvertExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Convert ); break; }
						case wabt::ExprType::Drop: { const auto& converted_expr = dynamic_cast<const wabt::DropExpr&>(expr); handleDrop(converted_expr); break; }
						case wabt::ExprType::GlobalGet: { const auto& converted_expr = dynamic_cast<const wabt::GlobalGetExpr&>(expr); handleGlobalGet(converted_expr); break; }
						case wabt::ExprType::GlobalSet: { const auto& converted_expr = dynamic_cast<const wabt::GlobalSetExpr&>(expr); handleGlobalSet(converted_expr); break; }
						case wabt::ExprType::If: { const auto& converted_expr = dynamic_cast<const wabt::IfExpr&>(expr); handleIf(converted_expr); break; }
						case wabt::ExprType::Load: { const auto& converted_expr = dynamic_cast<const wabt::LoadExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::Load ); break; }
						case wabt::ExprType::LocalGet: { const auto& converted_expr = dynamic_cast<const wabt::LocalGetExpr&>(expr); handleLocalGet(converted_expr); break; }
						case wabt::ExprType::LocalSet: { const auto& converted_expr = dynamic_cast<const wabt::LocalSetExpr&>(expr); handleLocalSet(converted_expr); break; }
						case wabt::ExprType::LocalTee: { const auto& converted_expr = dynamic_cast<const wabt::LocalTeeExpr&>(expr); handleLocalTee(converted_expr); break; }
						case wabt::ExprType::Loop: { const auto& converted_expr = dynamic_cast<const wabt::LoopExpr&>(expr); handleLoop(converted_expr); break; }
						case wabt::ExprType::MemoryCopy: { const auto& converted_expr = dynamic_cast<const wabt::MemoryCopyExpr&>(expr); handleMemoryCopy(converted_expr); break; }
						case wabt::ExprType::DataDrop: { const auto& converted_expr = dynamic_cast<const wabt::DataDropExpr&>(expr); handleDataDrop(converted_expr); break; }
						case wabt::ExprType::MemoryFill: { const auto& converted_expr = dynamic_cast<const wabt::MemoryFillExpr&>(expr); handleMemoryFill(converted_expr); break; }
						case wabt::ExprType::MemoryGrow: { const auto& converted_expr = dynamic_cast<const wabt::MemoryGrowExpr&>(expr); handleMemoryGrow(converted_expr); break; }
						case wabt::ExprType::MemoryInit: { const auto& converted_expr = dynamic_cast<const wabt::MemoryInitExpr&>(expr); handleMemoryInit(converted_expr); break; }
						case wabt::ExprType::MemorySize: { const auto& converted_expr = dynamic_cast<const wabt::MemorySizeExpr&>(expr); handleMemorySize(converted_expr); break; }
						case wabt::ExprType::Nop: { const auto& converted_expr = dynamic_cast<const wabt::NopExpr&>(expr); handleNop(converted_expr); break; }
						case wabt::ExprType::RefIsNull: { const auto& converted_expr = dynamic_cast<const wabt::RefIsNullExpr&>(expr); handleRefIsNull(converted_expr); break; }
						case wabt::ExprType::RefFunc: { const auto& converted_expr = dynamic_cast<const wabt::RefFuncExpr&>(expr); handleRefFunc(converted_expr); break; }
						case wabt::ExprType::RefNull: { const auto& converted_expr = dynamic_cast<const wabt::RefNullExpr&>(expr); handleRefNull(converted_expr); break; }
						case wabt::ExprType::Rethrow: { const auto& converted_expr = dynamic_cast<const wabt::RethrowExpr&>(expr); handleRethrow(converted_expr); break; }
						case wabt::ExprType::Return: { const auto& converted_expr = dynamic_cast<const wabt::ReturnExpr&>(expr); handleReturn(converted_expr); break; }
						case wabt::ExprType::ReturnCall: { const auto& converted_expr = dynamic_cast<const wabt::ReturnCallExpr&>(expr); handleReturnCall(converted_expr); break; }
						case wabt::ExprType::ReturnCallIndirect: { const auto& converted_expr = dynamic_cast<const wabt::ReturnCallIndirectExpr&>(expr); handleReturnCallIndirect(converted_expr); break; }
						case wabt::ExprType::Select: { const auto& converted_expr = dynamic_cast<const wabt::SelectExpr&>(expr); handleSelect(converted_expr); break; }
						case wabt::ExprType::SimdLaneOp: { const auto& converted_expr = dynamic_cast<const wabt::SimdLaneOpExpr&>(expr); handleSimdLaneOp(converted_expr); break; }
						case wabt::ExprType::SimdLoadLane: { const auto& converted_expr = dynamic_cast<const wabt::SimdLoadLaneExpr&>(expr); handleSimdLoadLane(converted_expr); break; }
						case wabt::ExprType::SimdStoreLane: { const auto& converted_expr = dynamic_cast<const wabt::SimdStoreLaneExpr&>(expr); handleSimdStoreLane(converted_expr); break; }
						case wabt::ExprType::SimdShuffleOp: { const auto& converted_expr = dynamic_cast<const wabt::SimdShuffleOpExpr&>(expr); handleSimdShuffleOp(converted_expr); break; }
						case wabt::ExprType::LoadSplat: { const auto& converted_expr = dynamic_cast<const wabt::LoadSplatExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::LoadSplat ); break; }
						case wabt::ExprType::LoadZero: { const auto& converted_expr = dynamic_cast<const wabt::LoadZeroExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::LoadZero ); break; }
						case wabt::ExprType::Store: { const auto& converted_expr = dynamic_cast<const wabt::StoreExpr&>(expr);
							handleLoadStore(converted_expr.loc,converted_expr.memidx, converted_expr.opcode, converted_expr.align, converted_expr.offset, LoadStoreType::Store ); break; }
						case wabt::ExprType::TableCopy: { const auto& converted_expr = dynamic_cast<const wabt::TableCopyExpr&>(expr); handleTableCopy(converted_expr); break; }
						case wabt::ExprType::ElemDrop: { const auto& converted_expr = dynamic_cast<const wabt::ElemDropExpr&>(expr); handleElemDrop(converted_expr); break; }
						case wabt::ExprType::TableInit: { const auto& converted_expr = dynamic_cast<const wabt::TableInitExpr&>(expr); handleTableInit(converted_expr); break; }
						case wabt::ExprType::TableGet: { const auto& converted_expr = dynamic_cast<const wabt::TableGetExpr&>(expr); handleTableGet(converted_expr); break; }
						case wabt::ExprType::TableGrow: { const auto& converted_expr = dynamic_cast<const wabt::TableGrowExpr&>(expr); handleTableGrow(converted_expr); break; }
						case wabt::ExprType::TableSize: { const auto& converted_expr = dynamic_cast<const wabt::TableSizeExpr&>(expr); handleTableSize(converted_expr); break; }
						case wabt::ExprType::TableSet: { const auto& converted_expr = dynamic_cast<const wabt::TableSetExpr&>(expr); handleTableSet(converted_expr); break; }
						case wabt::ExprType::TableFill: { const auto& converted_expr = dynamic_cast<const wabt::TableFillExpr&>(expr); handleTableFill(converted_expr); break; }
						case wabt::ExprType::Ternary: { const auto& converted_expr = dynamic_cast<const wabt::TernaryExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Ternary ); break; }
						case wabt::ExprType::Throw: { const auto& converted_expr = dynamic_cast<const wabt::ThrowExpr&>(expr); handleThrow(converted_expr); break; }
						case wabt::ExprType::ThrowRef: { const auto& converted_expr = dynamic_cast<const wabt::ThrowRefExpr&>(expr); handleThrowRef(converted_expr); break; }
						case wabt::ExprType::Try: { const auto& converted_expr = dynamic_cast<const wabt::TryExpr&>(expr); handleTry(converted_expr); break; }
						case wabt::ExprType::TryTable: { const auto& converted_expr = dynamic_cast<const wabt::TryTableExpr&>(expr); handleTryTable(converted_expr); break; }
						case wabt::ExprType::Unary: { const auto& converted_expr = dynamic_cast<const wabt::UnaryExpr&>(expr);
							handleOperator(converted_expr.loc, converted_expr.opcode, OperatorType::Unary ); break; }
						case wabt::ExprType::Unreachable: { const auto& converted_expr = dynamic_cast<const wabt::UnreachableExpr&>(expr); handleUnreachable(converted_expr); break; }
						default: break;
					}
				}
				break;
			}
			default:
				break;
		}
	}
};

#endif // WASMJITCONTEXT_HPP
