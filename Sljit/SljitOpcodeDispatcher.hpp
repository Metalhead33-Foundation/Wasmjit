#ifndef SLJITOPCODEDISPATCHER_HPP
#define SLJITOPCODEDISPATCHER_HPP

#include "../WasmBase/WasmModuleInstance.hpp"
#include "../WasmBase/WasmOpcodeDispatcher.hpp"

#include "../WasmBase/WasmBackendAbi.hpp"

#include <sljitLir.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*!
  \file SljitOpcodeDispatcher.hpp
  \brief Lowering one function body to SLJIT instructions.

  This header is internal: it is not part of the embedding surface.
*/

namespace Sljit {

//! Highest number of Wasm parameters one generated function can carry.
//! SLJIT accepts at most four arguments in `sljit_emit_enter` and in an emitted
//! call, and `VMContext*` is always the first of them.
inline constexpr std::uint32_t kMaxWasmParameters = 3;

//! Largest C argument-entry count the SLJIT register form can declare.
//!
//! `sljit_emit_enter` allocates one saved register per declared entry
//! (`function_check_arguments`, sljitLir.c), and the target provides four here.
//! The count includes `VMContext*`, every parameter word (a `v128` is two) and
//! the hidden out-buffer pointer a `v128` result needs. A signature above this
//! ceiling must use the frame form; the module compiler makes that decision
//! module-wide (docs/backends/STAGING_CALL_ABI.md).
inline constexpr std::uint32_t kRegisterArgumentCeiling = 4;

/*!
 * \brief Compiles one WebAssembly function body into SLJIT machine code.
 *
 * The emitter lowers the integer, control-flow, call, global and memory32
 * subset that the AsmJit backend also executes; a body outside it is refused
 * rather than half-emitted, so the core reports "no compiled body" at the call
 * boundary (docs/WRITING_A_BACKEND.md §9).
 *
 * Frame model: every Wasm local and every operand-stack slot is one machine
 * word in the function's local stack area, addressed as `SLJIT_MEM1(SLJIT_SP)`.
 * A few reserved slots follow the locals: the saved `VMContext*`, the indirect
 * call target and a scratch word that survives a helper call. Four scratch
 * registers (R0-R3) carry every operation and every C helper call, so register
 * pressure is constant and independent of the body. Parameters arrive in the
 * saved registers SLJIT assigns (`VMContext*` in S0, parameter *i* in S(i+1))
 * and are copied into their frame slots in the prologue.
 *
 * A function is compiled in two passes over the decoded body: the first
 * measures the operand-stack high-water mark (the frame cannot grow after
 * `sljit_emit_enter`), the second emits. Both passes share the same visit code,
 * so the measured depth cannot disagree with the emitted one.
 *
 * The emitter tracks a `reachable` flag per instruction. After `br`, `br_table`,
 * `return` or `unreachable`, ordinary instructions are skipped until a
 * structured `else`/`end` joins back into reachable code, which is what makes
 * the polymorphic stack of unreachable code safe to walk without emitting it.
 */
class OpcodeDispatcher : public WASM::OpcodeDispatcher
{
public:
	/*!
	 * \brief Prepares an emitter for one function body.
	 * \param module Module owning the body; borrowed and must outlive this object.
	 * \param funcType Declared signature of the function.
	 * \param body Decoded body whose opcode stream is walked.
	 * \param functionIndex Module function index, used for the trap-site annotation.
	 * \param frameCallAbi True to emit the staging-frame ABI (`void fn(VMContext*, void* frame)`) instead of the register form; set module-wide by the compiler (docs/backends/STAGING_CALL_ABI.md).
	 * \param tagIds The instance's tag index space, imports first, so a `throw tagidx` folds to its canonical id (docs/proposals/EXCEPTIONS.md EX-2).
	 * \param internals Instance callables, used to embed a direct tail-call target.
	 * \param profileConfig The active profile; its `N` marker enables deterministic NaN canonicalisation at observable boundaries (docs/proposals/PROFILES.md PR-3).
	 */
	OpcodeDispatcher(const WASM::Module& module, const WASM::FuncType& funcType,
			const WASM::FunctionBody& body, std::uint32_t functionIndex,
			bool frameCallAbi, const std::vector<WASM::TagId>& tagIds,
			WASM::ModuleInstanceInternals& internals,
			WASM::ProfileConfig profileConfig);

	/*!
	 * \brief Emits the function and hands back its executable entry point.
	 *
	 * Never throws for a body-caused condition: a construct outside the
	 * supported subset is reported by returning false, with the reason in
	 * unsupportedReason(). The caller leaves `Callable::fnPtr` null then.
	 *
	 * \param entry Receives the entry point on success; untouched on failure.
	 * \return true when the whole body was lowered to machine code.
	 */
	bool compile(void*& entry);

	/*!
	 * \brief Why the last compile() refused the function.
	 * \return A human-readable reason, or an empty string after a success.
	 */
	const std::string& unsupportedReason() const { return unsupportedReason_; }

protected:
	/*!
	 * \brief Refuses a function whose body uses an opcode this backend does not lower.
	 * \param name Dispatcher method name of the unhandled opcode.
	 */
	void onUnhandledOpcode(const char* name) override;

	void dispatchNop() override; //!< `nop`: nothing to emit.
	void dispatchUnreachable() override; //!< `unreachable`: report the trap and stop.
	void dispatchBlock(const WASM::BlockType& arg) override; //!< `block`.
	void dispatchLoop(const WASM::BlockType& arg) override; //!< `loop`.
	void dispatchIf(const WASM::BlockType& arg) override; //!< `if`.
	void dispatchElse() override; //!< `else`.
	void dispatchEnd() override; //!< `end` of a block or of the function.
	void dispatchBr(WASM::LabelIdx arg) override; //!< `br`.
	void dispatchBrIf(WASM::LabelIdx arg) override; //!< `br_if`.
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) override; //!< `br_table`.
	void dispatchReturn() override; //!< `return`.
	void dispatchReturnCall(WASM::FuncIdx arg) override; //!< `return_call`.
	void dispatchCallRef(WASM::TypeIdx arg) override; //!< `call_ref`.
	void dispatchTableGet(WASM::TableIdx arg) override; //!< `table.get`.
	void dispatchTableSet(WASM::TableIdx arg) override; //!< `table.set`.
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override; //!< `table.atomic.get`.
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override; //!< `table.atomic.set`.
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override; //!< `table.atomic.rmw.xchg`.
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override; //!< `table.atomic.rmw.cmpxchg`.
	void dispatchReturnCallRef(WASM::TypeIdx arg) override; //!< `return_call_ref`.
	void dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< `return_call_indirect`.
	void dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) override; //!< `try_table`.
	void dispatchThrow(WASM::TagIdx arg) override; //!< `throw`.
	void dispatchThrowRef() override; //!< `throw_ref`.
	void dispatchCall(WASM::FuncIdx arg) override; //!< `call`.
	void dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< `call_indirect`.
	void dispatchDrop() override; //!< `drop`.
	void dispatchSelect() override; //!< Untyped `select`.
	void dispatchSelectT(std::vector<WASM::ValueType>&& arg) override; //!< Typed `select`.
	void dispatchLocalGet(WASM::LocalIdx arg) override; //!< `local.get`.
	void dispatchLocalSet(WASM::LocalIdx arg) override; //!< `local.set`.
	void dispatchLocalTee(WASM::LocalIdx arg) override; //!< `local.tee`.
	void dispatchGlobalGet(WASM::GlobalIdx arg) override; //!< `global.get`.
	void dispatchGlobalSet(WASM::GlobalIdx arg) override; //!< `global.set`.
	void dispatchI32Const(std::int32_t arg) override; //!< `i32.const`.
	void dispatchI64Const(std::int64_t arg) override; //!< `i64.const`.

	void dispatchI32Add() override; //!< `i32.add`.
	void dispatchI32Sub() override; //!< `i32.sub`.
	void dispatchI32Mul() override; //!< `i32.mul`.
	void dispatchI32DivS() override; //!< `i32.div_s`.
	void dispatchI32DivU() override; //!< `i32.div_u`.
	void dispatchI32RemS() override; //!< `i32.rem_s`.
	void dispatchI32RemU() override; //!< `i32.rem_u`.
	void dispatchI32And() override; //!< `i32.and`.
	void dispatchI32Or() override; //!< `i32.or`.
	void dispatchI32Xor() override; //!< `i32.xor`.
	void dispatchI32Shl() override; //!< `i32.shl`.
	void dispatchI32ShrS() override; //!< `i32.shr_s`.
	void dispatchI32ShrU() override; //!< `i32.shr_u`.
	void dispatchI32Rotl() override; //!< `i32.rotl`.
	void dispatchI32Rotr() override; //!< `i32.rotr`.
	void dispatchI32Clz() override; //!< `i32.clz`.
	void dispatchI32Ctz() override; //!< `i32.ctz`.
	void dispatchI32Popcnt() override; //!< `i32.popcnt`.
	void dispatchI32Eqz() override; //!< `i32.eqz`.
	void dispatchI32Eq() override; //!< `i32.eq`.
	void dispatchI32Ne() override; //!< `i32.ne`.
	void dispatchI32LtS() override; //!< `i32.lt_s`.
	void dispatchI32LtU() override; //!< `i32.lt_u`.
	void dispatchI32GtS() override; //!< `i32.gt_s`.
	void dispatchI32GtU() override; //!< `i32.gt_u`.
	void dispatchI32LeS() override; //!< `i32.le_s`.
	void dispatchI32LeU() override; //!< `i32.le_u`.
	void dispatchI32GeS() override; //!< `i32.ge_s`.
	void dispatchI32GeU() override; //!< `i32.ge_u`.
	void dispatchI32Extend8S() override; //!< `i32.extend8_s`.
	void dispatchI32Extend16S() override; //!< `i32.extend16_s`.
	void dispatchI32WrapI64() override; //!< `i32.wrap_i64`.

	void dispatchI64Add() override; //!< `i64.add`.
	void dispatchI64Sub() override; //!< `i64.sub`.
	void dispatchI64Mul() override; //!< `i64.mul`.
	void dispatchI64Add128() override; //!< `i64.add128`.
	void dispatchI64Sub128() override; //!< `i64.sub128`.
	void dispatchI64MulWideS() override; //!< `i64.mul_wide_s`.
	void dispatchI64MulWideU() override; //!< `i64.mul_wide_u`.
	void dispatchI64DivS() override; //!< `i64.div_s`.
	void dispatchI64DivU() override; //!< `i64.div_u`.
	void dispatchI64RemS() override; //!< `i64.rem_s`.
	void dispatchI64RemU() override; //!< `i64.rem_u`.
	void dispatchI64And() override; //!< `i64.and`.
	void dispatchI64Or() override; //!< `i64.or`.
	void dispatchI64Xor() override; //!< `i64.xor`.
	void dispatchI64Shl() override; //!< `i64.shl`.
	void dispatchI64ShrS() override; //!< `i64.shr_s`.
	void dispatchI64ShrU() override; //!< `i64.shr_u`.
	void dispatchI64Rotl() override; //!< `i64.rotl`.
	void dispatchI64Rotr() override; //!< `i64.rotr`.
	void dispatchI64Clz() override; //!< `i64.clz`.
	void dispatchI64Ctz() override; //!< `i64.ctz`.
	void dispatchI64Popcnt() override; //!< `i64.popcnt`.
	void dispatchI64Eqz() override; //!< `i64.eqz`.
	void dispatchI64Eq() override; //!< `i64.eq`.
	void dispatchI64Ne() override; //!< `i64.ne`.
	void dispatchI64LtS() override; //!< `i64.lt_s`.
	void dispatchI64LtU() override; //!< `i64.lt_u`.
	void dispatchI64GtS() override; //!< `i64.gt_s`.
	void dispatchI64GtU() override; //!< `i64.gt_u`.
	void dispatchI64LeS() override; //!< `i64.le_s`.
	void dispatchI64LeU() override; //!< `i64.le_u`.
	void dispatchI64GeS() override; //!< `i64.ge_s`.
	void dispatchI64GeU() override; //!< `i64.ge_u`.
	void dispatchI64Extend8S() override; //!< `i64.extend8_s`.
	void dispatchI64Extend16S() override; //!< `i64.extend16_s`.
	void dispatchI64Extend32S() override; //!< `i64.extend32_s`.
	void dispatchI64ExtendI32S() override; //!< `i64.extend_i32_s`.
	void dispatchI64ExtendI32U() override; //!< `i64.extend_i32_u`.

	void dispatchF32Const(float arg) override; //!< `f32.const`.
	void dispatchF64Const(double arg) override; //!< `f64.const`.
	void dispatchF32Abs() override; //!< `f32.abs`.
	void dispatchF32Neg() override; //!< `f32.neg`.
	void dispatchF32Sqrt() override; //!< `f32.sqrt`.
	void dispatchF32Ceil() override; //!< `f32.ceil`.
	void dispatchF32Floor() override; //!< `f32.floor`.
	void dispatchF32Trunc() override; //!< `f32.trunc`.
	void dispatchF32Nearest() override; //!< `f32.nearest`.
	void dispatchF32Add() override; //!< `f32.add`.
	void dispatchF32Sub() override; //!< `f32.sub`.
	void dispatchF32Mul() override; //!< `f32.mul`.
	void dispatchF32Div() override; //!< `f32.div`.
	void dispatchF32Min() override; //!< `f32.min`.
	void dispatchF32Max() override; //!< `f32.max`.
	void dispatchF32Copysign() override; //!< `f32.copysign`.
	void dispatchF32Eq() override; //!< `f32.eq`.
	void dispatchF32Ne() override; //!< `f32.ne`.
	void dispatchF32Lt() override; //!< `f32.lt`.
	void dispatchF32Gt() override; //!< `f32.gt`.
	void dispatchF32Le() override; //!< `f32.le`.
	void dispatchF32Ge() override; //!< `f32.ge`.
	void dispatchF64Abs() override; //!< `f64.abs`.
	void dispatchF64Neg() override; //!< `f64.neg`.
	void dispatchF64Sqrt() override; //!< `f64.sqrt`.
	void dispatchF64Ceil() override; //!< `f64.ceil`.
	void dispatchF64Floor() override; //!< `f64.floor`.
	void dispatchF64Trunc() override; //!< `f64.trunc`.
	void dispatchF64Nearest() override; //!< `f64.nearest`.
	void dispatchF64Add() override; //!< `f64.add`.
	void dispatchF64Sub() override; //!< `f64.sub`.
	void dispatchF64Mul() override; //!< `f64.mul`.
	void dispatchF64Div() override; //!< `f64.div`.
	void dispatchF64Min() override; //!< `f64.min`.
	void dispatchF64Max() override; //!< `f64.max`.
	void dispatchF64Copysign() override; //!< `f64.copysign`.
	void dispatchF64Eq() override; //!< `f64.eq`.
	void dispatchF64Ne() override; //!< `f64.ne`.
	void dispatchF64Lt() override; //!< `f64.lt`.
	void dispatchF64Gt() override; //!< `f64.gt`.
	void dispatchF64Le() override; //!< `f64.le`.
	void dispatchF64Ge() override; //!< `f64.ge`.
	void dispatchF32ConvertI32S() override; //!< `f32.convert_i32_s`.
	void dispatchF32ConvertI32U() override; //!< `f32.convert_i32_u`.
	void dispatchF32ConvertI64S() override; //!< `f32.convert_i64_s`.
	void dispatchF32ConvertI64U() override; //!< `f32.convert_i64_u`.
	void dispatchF64ConvertI32S() override; //!< `f64.convert_i32_s`.
	void dispatchF64ConvertI32U() override; //!< `f64.convert_i32_u`.
	void dispatchF64ConvertI64S() override; //!< `f64.convert_i64_s`.
	void dispatchF64ConvertI64U() override; //!< `f64.convert_i64_u`.
	void dispatchF32DemoteF64() override; //!< `f32.demote_f64`.
	void dispatchF64PromoteF32() override; //!< `f64.promote_f32`.
	void dispatchI32TruncF32S() override; //!< `i32.trunc_f32_s`.
	void dispatchI32TruncF32U() override; //!< `i32.trunc_f32_u`.
	void dispatchI32TruncF64S() override; //!< `i32.trunc_f64_s`.
	void dispatchI32TruncF64U() override; //!< `i32.trunc_f64_u`.
	void dispatchI64TruncF32S() override; //!< `i64.trunc_f32_s`.
	void dispatchI64TruncF32U() override; //!< `i64.trunc_f32_u`.
	void dispatchI64TruncF64S() override; //!< `i64.trunc_f64_s`.
	void dispatchI64TruncF64U() override; //!< `i64.trunc_f64_u`.
	void dispatchI32TruncSatF32S() override; //!< `i32.trunc_sat_f32_s`.
	void dispatchI32TruncSatF32U() override; //!< `i32.trunc_sat_f32_u`.
	void dispatchI32TruncSatF64S() override; //!< `i32.trunc_sat_f64_s`.
	void dispatchI32TruncSatF64U() override; //!< `i32.trunc_sat_f64_u`.
	void dispatchI64TruncSatF32S() override; //!< `i64.trunc_sat_f32_s`.
	void dispatchI64TruncSatF32U() override; //!< `i64.trunc_sat_f32_u`.
	void dispatchI64TruncSatF64S() override; //!< `i64.trunc_sat_f64_s`.
	void dispatchI64TruncSatF64U() override; //!< `i64.trunc_sat_f64_u`.
	void dispatchI32ReinterpretF32() override; //!< `i32.reinterpret_f32`.
	void dispatchI64ReinterpretF64() override; //!< `i64.reinterpret_f64`.
	void dispatchF32ReinterpretI32() override; //!< `f32.reinterpret_i32`.
	void dispatchF64ReinterpretI64() override; //!< `f64.reinterpret_i64`.
	void dispatchF32Load(WASM::MemArg arg) override; //!< `f32.load`.
	void dispatchF64Load(WASM::MemArg arg) override; //!< `f64.load`.
	void dispatchF32Store(WASM::MemArg arg) override; //!< `f32.store`.
	void dispatchF64Store(WASM::MemArg arg) override; //!< `f64.store`.

	void dispatchV128Const(std::span<uint8_t> arg) override; //!< `v128.const`.
	void dispatchV128Load(WASM::MemArg arg) override; //!< `v128.load`.
	void dispatchV128Store(WASM::MemArg arg) override; //!< `v128.store`.
	void dispatchI8x16Splat() override; //!< `i8x16.splat`.
	void dispatchI16x8Splat() override; //!< `i16x8.splat`.
	void dispatchI64x2Splat() override; //!< `i64x2.splat`.
	void dispatchI32x4Splat() override; //!< `i32x4.splat`.
	void dispatchI8x16ExtractLaneS(uint8_t arg) override; //!< `i8x16.extract_lane_s`.
	void dispatchI8x16ExtractLaneU(uint8_t arg) override; //!< `i8x16.extract_lane_u`.
	void dispatchI16x8ExtractLaneS(uint8_t arg) override; //!< `i16x8.extract_lane_s`.
	void dispatchI16x8ExtractLaneU(uint8_t arg) override; //!< `i16x8.extract_lane_u`.
	void dispatchI64x2ExtractLane(uint8_t arg) override; //!< `i64x2.extract_lane`.
	void dispatchI32x4ExtractLane(uint8_t arg) override; //!< `i32x4.extract_lane`.
	void dispatchI8x16ReplaceLane(uint8_t arg) override; //!< `i8x16.replace_lane`.
	void dispatchI16x8ReplaceLane(uint8_t arg) override; //!< `i16x8.replace_lane`.
	void dispatchI64x2ReplaceLane(uint8_t arg) override; //!< `i64x2.replace_lane`.
	void dispatchI32x4ReplaceLane(uint8_t arg) override; //!< `i32x4.replace_lane`.
	void dispatchV128Not() override; //!< `v128.not`.
	void dispatchV128And() override; //!< `v128.and`.
	void dispatchV128AndNot() override; //!< `v128.andnot`.
	void dispatchV128Or() override; //!< `v128.or`.
	void dispatchV128Xor() override; //!< `v128.xor`.
	void dispatchV128Bitselect() override; //!< `v128.bitselect`.
	void dispatchV128AnyTrue() override; //!< `v128.any_true`.
	void dispatchI8x16Shl() override; //!< `i8x16.shl`.
	void dispatchI8x16ShrS() override; //!< `i8x16.shr_s`.
	void dispatchI8x16ShrU() override; //!< `i8x16.shr_u`.
	void dispatchI16x8Shl() override; //!< `i16x8.shl`.
	void dispatchI16x8ShrS() override; //!< `i16x8.shr_s`.
	void dispatchI16x8ShrU() override; //!< `i16x8.shr_u`.
	void dispatchI32x4Shl() override; //!< `i32x4.shl`.
	void dispatchI32x4ShrS() override; //!< `i32x4.shr_s`.
	void dispatchI32x4ShrU() override; //!< `i32x4.shr_u`.
	void dispatchI64x2Shl() override; //!< `i64x2.shl`.
	void dispatchI64x2ShrS() override; //!< `i64x2.shr_s`.
	void dispatchI64x2ShrU() override; //!< `i64x2.shr_u`.
	void dispatchI8x16MinS() override; //!< `i8x16.min_s`.
	void dispatchI8x16MinU() override; //!< `i8x16.min_u`.
	void dispatchI8x16MaxS() override; //!< `i8x16.max_s`.
	void dispatchI8x16MaxU() override; //!< `i8x16.max_u`.
	void dispatchI16x8MinS() override; //!< `i16x8.min_s`.
	void dispatchI16x8MinU() override; //!< `i16x8.min_u`.
	void dispatchI16x8MaxS() override; //!< `i16x8.max_s`.
	void dispatchI16x8MaxU() override; //!< `i16x8.max_u`.
	void dispatchI32x4MinS() override; //!< `i32x4.min_s`.
	void dispatchI32x4MinU() override; //!< `i32x4.min_u`.
	void dispatchI32x4MaxS() override; //!< `i32x4.max_s`.
	void dispatchI32x4MaxU() override; //!< `i32x4.max_u`.
	void dispatchI8x16AddSatS() override; //!< `i8x16.add_sat_s`.
	void dispatchI8x16AddSatU() override; //!< `i8x16.add_sat_u`.
	void dispatchI8x16SubSatS() override; //!< `i8x16.sub_sat_s`.
	void dispatchI8x16SubSatU() override; //!< `i8x16.sub_sat_u`.
	void dispatchI16x8AddSatS() override; //!< `i16x8.add_sat_s`.
	void dispatchI16x8AddSatU() override; //!< `i16x8.add_sat_u`.
	void dispatchI16x8SubSatS() override; //!< `i16x8.sub_sat_s`.
	void dispatchI16x8SubSatU() override; //!< `i16x8.sub_sat_u`.
	void dispatchI16x8Q15MulRSatS() override; //!< `i16x8.q15mulr_sat_s`.
	void dispatchI8x16AvgrU() override; //!< `i8x16.avgr_u`.
	void dispatchI16x8AvgrU() override; //!< `i16x8.avgr_u`.
	void dispatchI8x16Abs() override; //!< `i8x16.abs`.
	void dispatchI16x8Abs() override; //!< `i16x8.abs`.
	void dispatchI32x4Abs() override; //!< `i32x4.abs`.
	void dispatchI64x2Abs() override; //!< `i64x2.abs`.
	void dispatchI8x16Popcnt() override; //!< `i8x16.popcnt`.
	void dispatchI16x8ExtendLowI8x16S() override; //!< `16x8.extend_low_8x16_s`.
	void dispatchI16x8ExtendLowI8x16U() override; //!< `16x8.extend_low_8x16_u`.
	void dispatchI16x8ExtendHighI8x16S() override; //!< `16x8.extend_high_8x16_s`.
	void dispatchI16x8ExtendHighI8x16U() override; //!< `16x8.extend_high_8x16_u`.
	void dispatchI32x4ExtendLowI16x8S() override; //!< `32x4.extend_low_16x8_s`.
	void dispatchI32x4ExtendLowI16x8U() override; //!< `32x4.extend_low_16x8_u`.
	void dispatchI32x4ExtendHighI16x8S() override; //!< `32x4.extend_high_16x8_s`.
	void dispatchI32x4ExtendHighI16x8U() override; //!< `32x4.extend_high_16x8_u`.
	void dispatchI64x2ExtendLowI32x4S() override; //!< `64x2.extend_low_32x4_s`.
	void dispatchI64x2ExtendLowI32x4U() override; //!< `64x2.extend_low_32x4_u`.
	void dispatchI64x2ExtendHighI32x4S() override; //!< `64x2.extend_high_32x4_s`.
	void dispatchI64x2ExtendHighI32x4U() override; //!< `64x2.extend_high_32x4_u`.
	void dispatchI16x8ExtAddPairwiseI8x16S() override; //!< `16x8.extadd_pairwise_8x16_s`.
	void dispatchI16x8ExtAddPairwiseI8x16U() override; //!< `16x8.extadd_pairwise_8x16_u`.
	void dispatchI32x4ExtAddPairwiseI16x8S() override; //!< `32x4.extadd_pairwise_16x8_s`.
	void dispatchI32x4ExtAddPairwiseI16x8U() override; //!< `32x4.extadd_pairwise_16x8_u`.
	void dispatchI16x8ExtMulLowI8x16S() override; //!< `16x8.extmul_low_8x16_s`.
	void dispatchI16x8ExtMulLowI8x16U() override; //!< `16x8.extmul_low_8x16_u`.
	void dispatchI16x8ExtMulHighI8x16S() override; //!< `16x8.extmul_high_8x16_s`.
	void dispatchI16x8ExtMulHighI8x16U() override; //!< `16x8.extmul_high_8x16_u`.
	void dispatchI32x4ExtMulLowI16x8S() override; //!< `32x4.extmul_low_16x8_s`.
	void dispatchI32x4ExtMulLowI16x8U() override; //!< `32x4.extmul_low_16x8_u`.
	void dispatchI32x4ExtMulHighI16x8S() override; //!< `32x4.extmul_high_16x8_s`.
	void dispatchI32x4ExtMulHighI16x8U() override; //!< `32x4.extmul_high_16x8_u`.
	void dispatchI64x2ExtMulLowI32x4S() override; //!< `64x2.extmul_low_32x4_s`.
	void dispatchI64x2ExtMulLowI32x4U() override; //!< `64x2.extmul_low_32x4_u`.
	void dispatchI64x2ExtMulHighI32x4S() override; //!< `64x2.extmul_high_32x4_s`.
	void dispatchI64x2ExtMulHighI32x4U() override; //!< `64x2.extmul_high_32x4_u`.
	void dispatchI8x16NarrowI16x8S() override; //!< `i8x16.narrow_i16x8_s`.
	void dispatchI8x16NarrowI16x8U() override; //!< `i8x16.narrow_i16x8_u`.
	void dispatchI16x8NarrowI32x4S() override; //!< `i16x8.narrow_i32x4_s`.
	void dispatchI16x8NarrowI32x4U() override; //!< `i16x8.narrow_i32x4_u`.
	void dispatchI32x4DotI16x8S() override; //!< `i32x4.dot_i16x8_s`.
	void dispatchF32x4Add() override; //!< `f32x4.add`.
	void dispatchF32x4Sub() override; //!< `f32x4.sub`.
	void dispatchF32x4Mul() override; //!< `f32x4.mul`.
	void dispatchF32x4Div() override; //!< `f32x4.div`.
	void dispatchF32x4Min() override; //!< `f32x4.min`.
	void dispatchF32x4Max() override; //!< `f32x4.max`.
	void dispatchF32x4PMin() override; //!< `f32x4.pmin`.
	void dispatchF32x4PMax() override; //!< `f32x4.pmax`.
	void dispatchF32x4Eq() override; //!< `f32x4.eq`.
	void dispatchF32x4Ne() override; //!< `f32x4.ne`.
	void dispatchF32x4Lt() override; //!< `f32x4.lt`.
	void dispatchF32x4Gt() override; //!< `f32x4.gt`.
	void dispatchF32x4Le() override; //!< `f32x4.le`.
	void dispatchF32x4Ge() override; //!< `f32x4.ge`.
	void dispatchF64x2Add() override; //!< `f64x2.add`.
	void dispatchF64x2Sub() override; //!< `f64x2.sub`.
	void dispatchF64x2Mul() override; //!< `f64x2.mul`.
	void dispatchF64x2Div() override; //!< `f64x2.div`.
	void dispatchF64x2Min() override; //!< `f64x2.min`.
	void dispatchF64x2Max() override; //!< `f64x2.max`.
	void dispatchF64x2PMin() override; //!< `f64x2.pmin`.
	void dispatchF64x2PMax() override; //!< `f64x2.pmax`.
	void dispatchF64x2Eq() override; //!< `f64x2.eq`.
	void dispatchF64x2Ne() override; //!< `f64x2.ne`.
	void dispatchF64x2Lt() override; //!< `f64x2.lt`.
	void dispatchF64x2Gt() override; //!< `f64x2.gt`.
	void dispatchF64x2Le() override; //!< `f64x2.le`.
	void dispatchF64x2Ge() override; //!< `f64x2.ge`.
	void dispatchF32x4Ceil() override; //!< `f32x4.ceil`.
	void dispatchF32x4Floor() override; //!< `f32x4.floor`.
	void dispatchF32x4Trunc() override; //!< `f32x4.trunc`.
	void dispatchF32x4Nearest() override; //!< `f32x4.nearest`.
	void dispatchF32x4Abs() override; //!< `f32x4.abs`.
	void dispatchF32x4Neg() override; //!< `f32x4.neg`.
	void dispatchF32x4Sqrt() override; //!< `f32x4.sqrt`.
	void dispatchF64x2Ceil() override; //!< `f64x2.ceil`.
	void dispatchF64x2Floor() override; //!< `f64x2.floor`.
	void dispatchF64x2Trunc() override; //!< `f64x2.trunc`.
	void dispatchF64x2Nearest() override; //!< `f64x2.nearest`.
	void dispatchF64x2Abs() override; //!< `f64x2.abs`.
	void dispatchF64x2Neg() override; //!< `f64x2.neg`.
	void dispatchF64x2Sqrt() override; //!< `f64x2.sqrt`.
	void dispatchI32x4TruncSatF32x4S() override; //!< `i32x4.trunc_sat_f32x4_s`.
	void dispatchI32x4TruncSatF32x4U() override; //!< `i32x4.trunc_sat_f32x4_u`.
	void dispatchF32x4ConvertI32x4S() override; //!< `f32x4.convert_i32x4_s`.
	void dispatchF32x4ConvertI32x4U() override; //!< `f32x4.convert_i32x4_u`.
	void dispatchF64x2ConvertLowI32x4S() override; //!< `f64x2.convert_low_i32x4_s`.
	void dispatchF64x2ConvertLowI32x4U() override; //!< `f64x2.convert_low_i32x4_u`.
	void dispatchF64x2PromoteLowF32x4() override; //!< `f64x2.promote_low_f32x4`.
	void dispatchI32x4TruncSatF64x2SZero() override; //!< `i32x4.trunc_sat_f64x2_s_zero`.
	void dispatchI32x4TruncSatF64x2UZero() override; //!< `i32x4.trunc_sat_f64x2_u_zero`.
	void dispatchF32x4DemoteF64x2Zero() override; //!< `f32x4.demote_f64x2_zero`.
	void dispatchF32x4Splat() override; //!< `f32x4.splat`.
	void dispatchF64x2Splat() override; //!< `f64x2.splat`.
	void dispatchF32x4ExtractLane(uint8_t arg) override; //!< `f32x4.extract_lane`.
	void dispatchF32x4ReplaceLane(uint8_t arg) override; //!< `f32x4.replace_lane`.
	void dispatchF64x2ExtractLane(uint8_t arg) override; //!< `f64x2.extract_lane`.
	void dispatchF64x2ReplaceLane(uint8_t arg) override; //!< `f64x2.replace_lane`.
	void dispatchV128Load8x8S(WASM::MemArg arg) override; //!< `v128.load8x8_s`.
	void dispatchV128Load8x8U(WASM::MemArg arg) override; //!< `v128.load8x8_u`.
	void dispatchV128Load16x4S(WASM::MemArg arg) override; //!< `v128.load16x4_s`.
	void dispatchV128Load16x4U(WASM::MemArg arg) override; //!< `v128.load16x4_u`.
	void dispatchV128Load32x2S(WASM::MemArg arg) override; //!< `v128.load32x2_s`.
	void dispatchV128Load32x2U(WASM::MemArg arg) override; //!< `v128.load32x2_u`.
	void dispatchV128Load8Splat(WASM::MemArg arg) override; //!< `v128.load8_splat`.
	void dispatchV128Load16Splat(WASM::MemArg arg) override; //!< `v128.load16_splat`.
	void dispatchV128Load32Splat(WASM::MemArg arg) override; //!< `v128.load32_splat`.
	void dispatchV128Load64Splat(WASM::MemArg arg) override; //!< `v128.load64_splat`.
	void dispatchV128Load32Zero(WASM::MemArg arg) override; //!< `v128.load32_zero`.
	void dispatchV128Load64Zero(WASM::MemArg arg) override; //!< `v128.load64_zero`.
	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.load8_lane`.
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.load16_lane`.
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.load32_lane`.
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.load64_lane`.
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.store8_lane`.
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.store16_lane`.
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.store32_lane`.
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< `v128.store64_lane`.
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override; //!< `i8x16.shuffle`.
	void dispatchI8x16Swizzle() override; //!< `i8x16.swizzle`.
	void dispatchI8x16RelaxedSwizzle() override; //!< `i8x16.relaxed_swizzle`.
	void dispatchI32x4RelaxedTruncF32x4S() override; //!< `i32x4.relaxed_trunc_f32x4_s`.
	void dispatchI32x4RelaxedTruncF32x4U() override; //!< `i32x4.relaxed_trunc_f32x4_u`.
	void dispatchI32x4RelaxedTruncF64x2SZero() override; //!< `i32x4.relaxed_trunc_f64x2_s_zero`.
	void dispatchI32x4RelaxedTruncF64x2UZero() override; //!< `i32x4.relaxed_trunc_f64x2_u_zero`.
	void dispatchF32x4RelaxedMadd() override; //!< `f32x4.relaxed_madd`.
	void dispatchF32x4RelaxedNmadd() override; //!< `f32x4.relaxed_nmadd`.
	void dispatchF64x2RelaxedMadd() override; //!< `f64x2.relaxed_madd`.
	void dispatchF64x2RelaxedNmadd() override; //!< `f64x2.relaxed_nmadd`.
	void dispatchI8x16RelaxedLaneselect() override; //!< `i8x16.relaxed_laneselect`.
	void dispatchI16x8RelaxedLaneselect() override; //!< `i16x8.relaxed_laneselect`.
	void dispatchI32x4RelaxedLaneselect() override; //!< `i32x4.relaxed_laneselect`.
	void dispatchI64x2RelaxedLaneselect() override; //!< `i64x2.relaxed_laneselect`.
	void dispatchF32x4RelaxedMin() override; //!< `f32x4.relaxed_min`.
	void dispatchF32x4RelaxedMax() override; //!< `f32x4.relaxed_max`.
	void dispatchF64x2RelaxedMin() override; //!< `f64x2.relaxed_min`.
	void dispatchF64x2RelaxedMax() override; //!< `f64x2.relaxed_max`.
	void dispatchI16x8RelaxedQ15MulRS() override; //!< `i16x8.relaxed_q15mulr_s`.
	void dispatchI16x8RelaxedDotI8x16I7x16S() override; //!< `i16x8.relaxed_dot_i8x16_i7x16_s`.
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override; //!< `i32x4.relaxed_dot_i8x16_i7x16_add_s`.
	void dispatchRefNull(const WASM::HeapType& arg) override; //!< `ref.null`.
	void dispatchRefIsNull() override; //!< `ref.is_null`.
	void dispatchRefI31() override; //!< `ref.i31`.
	void dispatchI31GetS() override; //!< `i31.get_s`.
	void dispatchI31GetU() override; //!< `i31.get_u`.
	void dispatchAnyConvertExtern() override; //!< `any.convert_extern`.
	void dispatchExternConvertAny() override; //!< `extern.convert_any`.
	void dispatchBrOnNull(WASM::LabelIdx arg) override; //!< `br_on_null`.
	void dispatchBrOnNonNull(WASM::LabelIdx arg) override; //!< `br_on_non_null`.
	void dispatchBrOnCast(uint8_t arg1, WASM::LabelIdx arg2, WASM::HeapType arg3,
			WASM::HeapType arg4) override; //!< `br_on_cast`.
	void dispatchBrOnCastFail(uint8_t arg1, WASM::LabelIdx arg2, WASM::HeapType arg3,
			WASM::HeapType arg4) override; //!< `br_on_cast_fail`.
	void dispatchStructNew(WASM::TypeIdx arg) override; //!< `struct.new`.
	void dispatchStructNewDefault(WASM::TypeIdx arg) override; //!< `struct.new_default`.
	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `struct.get`.
	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `struct.set`.
	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `struct.get_s`.
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `struct.get_u`.
	void dispatchArrayNew(WASM::TypeIdx arg) override; //!< `array.new`.
	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `array.new_data`.
	void dispatchArrayNewArray(WASM::TypeIdx dst, WASM::TypeIdx src) override; //!< `array.new_array`.
	void dispatchArrayNewMemory(WASM::TypeIdx dst, WASM::MemIdx src) override; //!< `array.new_memory`.
	void dispatchArrayNewTable(WASM::TypeIdx dst, WASM::TableIdx src) override; //!< `array.new_table`.
	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `array.new_elem`.
	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `array.init_data`.
	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `array.init_elem`.
	void dispatchArrayFill(WASM::TypeIdx arg) override; //!< `array.fill`.
	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override; //!< `array.copy`.
	void dispatchArrayNewDefault(WASM::TypeIdx arg) override; //!< `array.new_default`.
	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override; //!< `array.new_fixed`.
	void dispatchArrayGet(WASM::TypeIdx arg) override; //!< `array.get`.
	void dispatchArraySet(WASM::TypeIdx arg) override; //!< `array.set`.
	void dispatchArrayGetS(WASM::TypeIdx arg) override; //!< `array.get_s`.
	void dispatchArrayGetU(WASM::TypeIdx arg) override; //!< `array.get_u`.
	void dispatchArrayLen() override; //!< `array.len`.
	void dispatchRefFunc(WASM::FuncIdx arg) override; //!< `ref.func`.
	void dispatchRefAsNonNull() override; //!< `ref.as_non_null`.
	void dispatchRefTest(const WASM::HeapType& arg) override; //!< `ref.test`.
	void dispatchRefTestNull(const WASM::HeapType& arg) override; //!< `ref.test (ref null ...)`.
	void dispatchRefCast(const WASM::HeapType& arg) override; //!< `ref.cast`.
	void dispatchRefCastNull(const WASM::HeapType& arg) override; //!< `ref.cast (ref null ...)`.
	void dispatchRefEq() override; //!< `ref.eq`.
	void dispatchI8x16Add() override; //!< `i8x16.add`.
	void dispatchI8x16Sub() override; //!< `i8x16.sub`.
	void dispatchI16x8Add() override; //!< `i16x8.add`.
	void dispatchI16x8Sub() override; //!< `i16x8.sub`.
	void dispatchI16x8Mul() override; //!< `i16x8.mul`.
	void dispatchI32x4Add() override; //!< `i32x4.add`.
	void dispatchI32x4Sub() override; //!< `i32x4.sub`.
	void dispatchI32x4Mul() override; //!< `i32x4.mul`.
	void dispatchI64x2Add() override; //!< `i64x2.add`.
	void dispatchI64x2Sub() override; //!< `i64x2.sub`.
	void dispatchI64x2Mul() override; //!< `i64x2.mul`.
	void dispatchI8x16Eq() override; //!< `i8x16.eq`.
	void dispatchI8x16Ne() override; //!< `i8x16.ne`.
	void dispatchI8x16LtS() override; //!< `i8x16.lt_s`.
	void dispatchI8x16LtU() override; //!< `i8x16.lt_u`.
	void dispatchI8x16GtS() override; //!< `i8x16.gt_s`.
	void dispatchI8x16GtU() override; //!< `i8x16.gt_u`.
	void dispatchI8x16LeS() override; //!< `i8x16.le_s`.
	void dispatchI8x16LeU() override; //!< `i8x16.le_u`.
	void dispatchI8x16GeS() override; //!< `i8x16.ge_s`.
	void dispatchI8x16GeU() override; //!< `i8x16.ge_u`.
	void dispatchI16x8Eq() override; //!< `i16x8.eq`.
	void dispatchI16x8Ne() override; //!< `i16x8.ne`.
	void dispatchI16x8LtS() override; //!< `i16x8.lt_s`.
	void dispatchI16x8LtU() override; //!< `i16x8.lt_u`.
	void dispatchI16x8GtS() override; //!< `i16x8.gt_s`.
	void dispatchI16x8GtU() override; //!< `i16x8.gt_u`.
	void dispatchI16x8LeS() override; //!< `i16x8.le_s`.
	void dispatchI16x8LeU() override; //!< `i16x8.le_u`.
	void dispatchI16x8GeS() override; //!< `i16x8.ge_s`.
	void dispatchI16x8GeU() override; //!< `i16x8.ge_u`.
	void dispatchI32x4Eq() override; //!< `i32x4.eq`.
	void dispatchI32x4Ne() override; //!< `i32x4.ne`.
	void dispatchI32x4LtS() override; //!< `i32x4.lt_s`.
	void dispatchI32x4LtU() override; //!< `i32x4.lt_u`.
	void dispatchI32x4GtS() override; //!< `i32x4.gt_s`.
	void dispatchI32x4GtU() override; //!< `i32x4.gt_u`.
	void dispatchI32x4LeS() override; //!< `i32x4.le_s`.
	void dispatchI32x4LeU() override; //!< `i32x4.le_u`.
	void dispatchI32x4GeS() override; //!< `i32x4.ge_s`.
	void dispatchI32x4GeU() override; //!< `i32x4.ge_u`.
	void dispatchI64x2Eq() override; //!< `i64x2.eq`.
	void dispatchI64x2Ne() override; //!< `i64x2.ne`.
	void dispatchI64x2LtS() override; //!< `i64x2.lt_s`.
	void dispatchI64x2GtS() override; //!< `i64x2.gt_s`.
	void dispatchI64x2LeS() override; //!< `i64x2.le_s`.
	void dispatchI64x2GeS() override; //!< `i64x2.ge_s`.
	void dispatchI8x16Neg() override; //!< `i8x16.neg`.
	void dispatchI16x8Neg() override; //!< `i16x8.neg`.
	void dispatchI32x4Neg() override; //!< `i32x4.neg`.
	void dispatchI64x2Neg() override; //!< `i64x2.neg`.
	void dispatchI8x16AllTrue() override; //!< `i8x16.all_true`.
	void dispatchI16x8AllTrue() override; //!< `i16x8.all_true`.
	void dispatchI32x4AllTrue() override; //!< `i32x4.all_true`.
	void dispatchI64x2AllTrue() override; //!< `i64x2.all_true`.
	void dispatchI8x16Bitmask() override; //!< `i8x16.bitmask`.
	void dispatchI16x8Bitmask() override; //!< `i16x8.bitmask`.
	void dispatchI32x4Bitmask() override; //!< `i32x4.bitmask`.
	void dispatchI64x2Bitmask() override; //!< `i64x2.bitmask`.

	void dispatchI32Load(WASM::MemArg arg) override; //!< `i32.load`.
	void dispatchI32Load8S(WASM::MemArg arg) override; //!< `i32.load8_s`.
	void dispatchI32Load8U(WASM::MemArg arg) override; //!< `i32.load8_u`.
	void dispatchI32Load16S(WASM::MemArg arg) override; //!< `i32.load16_s`.
	void dispatchI32Load16U(WASM::MemArg arg) override; //!< `i32.load16_u`.
	void dispatchI32Store(WASM::MemArg arg) override; //!< `i32.store`.
	void dispatchI32Store8(WASM::MemArg arg) override; //!< `i32.store8`.
	void dispatchI32Store16(WASM::MemArg arg) override; //!< `i32.store16`.
	void dispatchI64Load(WASM::MemArg arg) override; //!< `i64.load`.
	void dispatchI64Load8S(WASM::MemArg arg) override; //!< `i64.load8_s`.
	void dispatchI64Load8U(WASM::MemArg arg) override; //!< `i64.load8_u`.
	void dispatchI64Load16S(WASM::MemArg arg) override; //!< `i64.load16_s`.
	void dispatchI64Load16U(WASM::MemArg arg) override; //!< `i64.load16_u`.
	void dispatchI64Load32S(WASM::MemArg arg) override; //!< `i64.load32_s`.
	void dispatchI64Load32U(WASM::MemArg arg) override; //!< `i64.load32_u`.
	void dispatchI64Store(WASM::MemArg arg) override; //!< `i64.store`.
	void dispatchI64Store8(WASM::MemArg arg) override; //!< `i64.store8`.
	void dispatchI64Store16(WASM::MemArg arg) override; //!< `i64.store16`.
	void dispatchI64Store32(WASM::MemArg arg) override; //!< `i64.store32`.
	void dispatchMemorySize(WASM::MemIdx arg) override; //!< `memory.size`.
	void dispatchMemoryGrow(WASM::MemIdx arg) override; //!< `memory.grow`.
	void dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) override; //!< `memory.copy`.
	void dispatchMemoryFill(WASM::MemIdx arg) override; //!< `memory.fill`.
	void dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) override; //!< `memory.init`.
	void dispatchDataDrop(uint32_t arg) override; //!< `data.drop`.
	void dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) override; //!< `table.init`.
	void dispatchTableFill(WASM::TableIdx arg) override; //!< `table.fill`.
	void dispatchTableGrow(WASM::TableIdx arg) override; //!< `table.grow`.
	void dispatchElemDrop(uint32_t arg) override; //!< `elem.drop`.
	void dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) override; //!< `table.copy`.
	void dispatchTableSize(WASM::TableIdx arg) override; //!< `table.size`.

	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override;
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override;
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

	void dispatchI32AtomicLoad(WASM::MemArg arg) override; //!< Atomic load (4 byte(s), 4-byte value).
	void dispatchI64AtomicLoad(WASM::MemArg arg) override; //!< Atomic load (8 byte(s), 8-byte value).
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override; //!< Atomic load (1 byte(s), 4-byte value).
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override; //!< Atomic load (2 byte(s), 4-byte value).
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override; //!< Atomic load (1 byte(s), 8-byte value).
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override; //!< Atomic load (2 byte(s), 8-byte value).
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override; //!< Atomic load (4 byte(s), 8-byte value).
	void dispatchI32AtomicStore(WASM::MemArg arg) override; //!< Atomic store (4 byte(s), 4-byte value).
	void dispatchI64AtomicStore(WASM::MemArg arg) override; //!< Atomic store (8 byte(s), 8-byte value).
	void dispatchI32AtomicStore8(WASM::MemArg arg) override; //!< Atomic store (1 byte(s), 4-byte value).
	void dispatchI32AtomicStore16(WASM::MemArg arg) override; //!< Atomic store (2 byte(s), 4-byte value).
	void dispatchI64AtomicStore8(WASM::MemArg arg) override; //!< Atomic store (1 byte(s), 8-byte value).
	void dispatchI64AtomicStore16(WASM::MemArg arg) override; //!< Atomic store (2 byte(s), 8-byte value).
	void dispatchI64AtomicStore32(WASM::MemArg arg) override; //!< Atomic store (4 byte(s), 8-byte value).
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override; //!< Atomic `add` (4 byte(s)).
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override; //!< Atomic `add` (8 byte(s)).
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override; //!< Atomic `add` (1 byte(s)).
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override; //!< Atomic `add` (2 byte(s)).
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override; //!< Atomic `add` (1 byte(s)).
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override; //!< Atomic `add` (2 byte(s)).
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override; //!< Atomic `add` (4 byte(s)).
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override; //!< Atomic `sub` (4 byte(s)).
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override; //!< Atomic `sub` (8 byte(s)).
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override; //!< Atomic `sub` (1 byte(s)).
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override; //!< Atomic `sub` (2 byte(s)).
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override; //!< Atomic `sub` (1 byte(s)).
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override; //!< Atomic `sub` (2 byte(s)).
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override; //!< Atomic `sub` (4 byte(s)).
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override; //!< Atomic `and` (4 byte(s)).
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override; //!< Atomic `and` (8 byte(s)).
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override; //!< Atomic `and` (1 byte(s)).
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override; //!< Atomic `and` (2 byte(s)).
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override; //!< Atomic `and` (1 byte(s)).
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override; //!< Atomic `and` (2 byte(s)).
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override; //!< Atomic `and` (4 byte(s)).
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override; //!< Atomic `or` (4 byte(s)).
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override; //!< Atomic `or` (8 byte(s)).
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override; //!< Atomic `or` (1 byte(s)).
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override; //!< Atomic `or` (2 byte(s)).
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override; //!< Atomic `or` (1 byte(s)).
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override; //!< Atomic `or` (2 byte(s)).
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override; //!< Atomic `or` (4 byte(s)).
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override; //!< Atomic `xor` (4 byte(s)).
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override; //!< Atomic `xor` (8 byte(s)).
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override; //!< Atomic `xor` (1 byte(s)).
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override; //!< Atomic `xor` (2 byte(s)).
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override; //!< Atomic `xor` (1 byte(s)).
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override; //!< Atomic `xor` (2 byte(s)).
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override; //!< Atomic `xor` (4 byte(s)).
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override; //!< Atomic `xchg` (4 byte(s)).
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override; //!< Atomic `xchg` (8 byte(s)).
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Atomic `xchg` (1 byte(s)).
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Atomic `xchg` (2 byte(s)).
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Atomic `xchg` (1 byte(s)).
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Atomic `xchg` (2 byte(s)).
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override; //!< Atomic `xchg` (4 byte(s)).
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Atomic compare-exchange (4 byte(s)).
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Atomic compare-exchange (8 byte(s)).
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Atomic compare-exchange (1 byte(s)).
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Atomic compare-exchange (2 byte(s)).
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Atomic compare-exchange (1 byte(s)).
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Atomic compare-exchange (2 byte(s)).
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override; //!< Atomic compare-exchange (4 byte(s)).
	void dispatchMemoryAtomicNotify(WASM::MemArg arg) override; //!< `memory.atomic.notify`.
	void dispatchMemoryAtomicWait32(WASM::MemArg arg) override; //!< `memory.atomic.wait32`.
	void dispatchMemoryAtomicWait64(WASM::MemArg arg) override; //!< `memory.atomic.wait64`.
	void dispatchAtomicFence(uint8_t ordering) override; //!< `atomic.fence`.
	void dispatchAtomicPause() override; //!< `atomic.pause`.

private:
	//! Structured control kind of one active block frame.
	enum class ControlKind { Block, Loop, If };

	//! One entry of the structured-control stack.
	struct ControlFrame {
		ControlKind kind = ControlKind::Block; //!< Block, loop or if.
		std::size_t baseDepth = 0; //!< Operand depth the label's values live at.
		std::vector<int> paramWidths; //!< Widths of the block's parameters.
		std::vector<int> resultWidths; //!< Widths of the block's results.
		//! True for a `try_table` frame; its clauses route a pending exception.
		bool tryTable = false;
		//! Decoded catch clauses of a `try_table`, in source order.
		std::vector<WASM::CatchClause> catchClauses;
		sljit_label* begin = nullptr; //!< Branch target of a loop.
		sljit_jump* alternateJump = nullptr; //!< False path of an `if`.
		std::vector<sljit_jump*> endJumps; //!< Branches to the merge point.
		bool hasElse = false; //!< True once `else` was seen.
		bool entryReachable = false; //!< True when the block was entered in live code.
		bool thenReachable = false; //!< True when the then arm ended in live code.
		bool endTargeted = false; //!< True once a `br` targets `end`.
	};

	//! Parameter and result widths of one block type.
	struct BlockSignature {
		std::vector<int> params; //!< Parameter widths, in order.
		std::vector<int> results; //!< Result widths, in order.
	};

	/*!
	 * \brief Runs one pass over the body.
	 * \param measure true for the depth-measuring pass (no code is emitted).
	 * \return true when the whole body was visited and lowered.
	 */
	bool run(bool measure);
	/*!
	 * \brief Emits the prologue: frame, stack guard, argument copy and local zeroing.
	 * \return true when the prologue was emitted successfully.
	 */
	bool beginFunction();
	/*!
	 * \brief Lowers a direct `return_call` to this function as a loop.
	 *
	 * The arguments are written into the parameter locals, the non-parameter
	 * locals are reset to zero, and control branches to the body start. The
	 * frame is reused, so a self-recursive tail call runs in constant native
	 * stack.
	 */
	void emitSelfTailCall();
	/*!
	 * \brief Lowers a direct cross-function tail call to a pending dispatch.
	 *
	 * The callee and its arguments are written to `TailCallState`; the function
	 * then returns a dummy result. The libffi driver wrapping this function
	 * re-enters the target through its type thunk, so the native stack does not
	 * grow.
	 *
	 * \param arg Callee function index.
	 * \param type Callee signature.
	 */
	void emitTailCallState(WASM::FuncIdx arg, const WASM::FuncType& type);
	/*!
	 * \brief Lowers a cross-function `return_call_indirect` to a pending dispatch.
	 *
	 * The table slot is resolved and type-checked, then its `tailEntry` decides:
	 * a callable with a thunk writes `TailCallState` and returns; a foreign one
	 * falls back to the semantic call plus return.
	 *
	 * \param typeIndex Declared callee type.
	 * \param tableIndex Table index.
	 */
	void emitTailCallIndirectState(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex);
	/*!
	 * \brief Lowers a cross-function `return_call_ref` from a register target.
	 * \param type Callee signature.
	 * \param callableReg Register holding the `Callable*` reference.
	 */
	void emitTailCallStateFromReg(int callableReg, const WASM::FuncType& type);
	/*!
	 * \brief Traps when a reference register is null, continuing when non-null.
	 * \param callableReg Register holding the reference.
	 */
	void emitNullRefCheck(int callableReg);
	//! Emits the typed zero return a pending tail call leaves behind.
	void emitTailCallDummyReturn();
	/*! \brief Stores a reference slot with the GC barrier; the address is in R0. \param ordering Atomic ordering. \param valueDepth Operand depth of the value. */
	void emitSharedAtomicRefStore(uint8_t ordering, std::size_t valueDepth);
	/*! \brief Exchanges a reference slot; the address is in R0. \param ordering Atomic ordering. \param valueDepth Operand depth of the value. \param resultDepth Operand depth receiving the old reference. */
	void emitSharedAtomicRefXchg(uint8_t ordering, std::size_t valueDepth, std::size_t resultDepth);
	/*! \brief Compare-exchanges a reference slot; the address is in R0. \param ordering Atomic ordering. \param expectedDepth Operand depth of the expected reference. \param replacementDepth Operand depth of the replacement. \param resultDepth Operand depth receiving the old reference. */
	void emitSharedAtomicRefCmpxchg(uint8_t ordering, std::size_t expectedDepth,
			std::size_t replacementDepth, std::size_t resultDepth);
	/*!
	 * \brief Emits the trap epilogue, finalises the code and returns its entry.
	 * \param entry Receives the executable entry point.
	 * \return true when SLJIT produced code.
	 */
	bool finish(void*& entry);
	//! Releases the compiler without producing code.
	void releaseCompiler();

	//! Records one SLJIT status; the first failure refuses the whole function.
	void emit(sljit_s32 status);
	/*!
	 * \brief Pushes one operand-stack slot and records its width.
	 * \param width 4 for an i32 slot, 8 for an i64 slot.
	 */
	void push(int width);
	/** \brief Marks the top operand as owning one string-family reference. */
	void markTopStringRef();
	/** \brief Tests the ownership bit of the current operand-stack top. \return true when `drop` must release it. */
	bool topIsStringRef() const;
	/** \brief Address of the fixed continuation scratch frame. \param reg Register receiving the address. */
	void continuationFrameAddress(int reg);
	/** \brief Resolves a continuation type index to its function type. \param type Continuation type index. \return The target function type. */
	WASM::FuncType continuationSignature(WASM::TypeIdx type) const;
	/*!
	 * \brief Pops one operand-stack slot.
	 * \return The width of the popped slot.
	 */
	int pop();
	/*!
	 * \brief Refuses the function unless `condition` holds.
	 * \param condition Tested condition.
	 * \param reason Message recorded when the condition fails.
	 */
	void require(bool condition, const char* reason);
	/*!
	 * \brief Refuses the function unless at least `count` operands are present.
	 * \param count Required operand-stack depth.
	 */
	void requireStack(std::size_t count);
	/*!
	 * \brief Refuses a local index outside the frame.
	 * \param index Local index to check.
	 * \param what Opcode name for the diagnostic.
	 */
	void requireLocal(std::size_t index, const char* what);
	//! Drops the operand stack back to the innermost frame's base and marks the
	//! rest of the current arm unreachable.
	void markUnreachable();

	/*!
	 * \brief Byte offset of a local slot in the frame.
	 * \param index Local index.
	 * \return Offset from `SLJIT_SP`.
	 */
	sljit_sw localSlotOffset(std::size_t index) const;
	/*!
	 * \brief Byte offset of an operand-stack slot in the frame.
	 * \param depth Zero-based operand-stack depth.
	 * \return Offset from `SLJIT_SP`.
	 */
	sljit_sw stackSlotOffset(std::size_t depth) const;
	/*!
	 * \brief Byte offset of the reserved `VMContext*` frame slot.
	 * \return Offset from `SLJIT_SP`.
	 */
	sljit_sw contextSlotOffset() const;
	/*!
	 * \brief Byte offset of the reserved indirect-call-target frame slot.
	 * \return Offset from `SLJIT_SP`.
	 */
	sljit_sw targetSlotOffset() const;
	/*!
	 * \brief Byte offset of the reserved helper-result scratch frame slot.
	 * \return Offset from `SLJIT_SP`.
	 */
	sljit_sw scratchSlotOffset() const;
	//! Reserved slot holding the caller-supplied out-buffer for a `v128` result
	//! (docs/ABI_REFACTOR_STUDY.md §25).
	/** \brief Offset of the reserved slot holding the caller-supplied result buffer. \return Frame offset in bytes. */
	sljit_sw resultBufferSlotOffset() const;
	//! Offset of the reserved slot holding the caller's staging-frame pointer.
	/** \brief Offset of the reserved slot holding the frame pointer. \return Frame offset in bytes. */
	sljit_sw frameSlotOffset() const;
	/** \brief Offset of the staging scratch area used to build a callee frame. \return Frame offset in bytes. */
	sljit_sw callFrameOffset() const;
	//! True when this module compiles to the staging-frame ABI
	//! (`docs/backends/STAGING_CALL_ABI.md`): `void fn(VMContext*, void* frame)`,
	//! with every parameter and result in the caller's frame.
	bool frameAbi_ = false;
	//! Parameter kinds in declaration order, for the frame offsets.
	std::vector<WASM::ValueKind> frameKinds_;
	//! Byte offset of the result block inside the frame, after the argument block.
	sljit_sw frameResultOffset_ = 0;
	//! True when the active profile excludes the `N` marker: a scalar float that
	//! becomes observable must be replaced by the canonical NaN
	//! (docs/proposals/PROFILES.md PR-3).
	bool deterministicFloats_ = false;
	//! True while `dispatchResume` is being driven by `resume_throw*`.
	bool resumeThrowActive_ = false;
	sljit_sw payloadSlotOffset(std::size_t index = 0) const;
	//! Offset of the two-word shuffle selector block.
	sljit_sw selectorBlockOffset() const;

	/*!
	 * \brief Loads one operand-stack slot into a register.
	 * \param reg Destination scratch register.
	 * \param depth Zero-based operand-stack depth.
	 * \param width 4 for an i32 slot, 8 for an i64 slot.
	 */
	void loadSlot(int reg, std::size_t depth, int width);
	/*!
	 * \brief Stores a register into one operand-stack slot.
	 * \param reg Source scratch register.
	 * \param depth Zero-based operand-stack depth.
	 * \param width 4 for an i32 slot, 8 for an i64 slot.
	 */
	void storeSlot(int reg, std::size_t depth, int width);
	/*!
	 * \brief Loads the saved `VMContext*` into a register.
	 * \param reg Destination scratch register.
	 */
	void loadContext(int reg);
	/*!
	 * \brief Moves `widths.size()` operand slots ending at `endDepth` down to `base`.
	 * \param widths Widths of the values to move, in stack order.
	 * \param base Destination depth.
	 * \param endDepth Depth just past the source range.
	 */
	void transferToBase(const std::vector<int>& widths, std::size_t base,
			std::size_t endDepth);
	//! Emits the return of the function's single result (peeking at the stack top).
	void emitReturnValue();

	/*!
	 * \brief Calls one C helper through the host ABI.
	 * \tparam Function Helper function type.
	 * \param argTypes SLJIT argument description of the helper.
	 * \param address Helper entry point.
	 */
	template <typename Function>
	void emitHelper(sljit_s32 argTypes, Function address)
	{
		if (measureOnly_)
			return;
		emit(sljit_emit_icall(compiler_, SLJIT_CALL, argTypes, SLJIT_IMM,
				reinterpret_cast<sljit_sw>(address)));
	}
	//! Probes the pending-trap slot and branches to the trap epilogue when set.
	void emitPendingTrapCheck();
	/*!
	 * \brief Resolves the payload signature a tag index declares.
	 * \param tagIdx Tag index in the module's index space (imports first).
	 * \return The tag's function type, whose parameters are the payload.
	 */
	WASM::FuncType tagPayloadType(WASM::TagIdx tagIdx);
	/*!
	 * \brief Returns the canonical id of the tag a catch clause names.
	 * \param clause Decoded catch clause.
	 * \return The tag's process-wide id.
	 */
	WASM::TagId tagIdForClause(const WASM::CatchClause& clause);
	//! Probes the pending-exception slot after a call: routes a pending exception
	//! to a local `try_table` clause or returns from this function, and leaves the
	//! continuation reachable when the slot is empty.
	void emitPendingExceptionCheck();
	//! Dispatches the pending exception over the open `try_table` frames at run
	//! time; every emitted path branches to a catch target or returns zeroes.
	void emitExceptionPropagation();
	/*!
	 * \brief Writes a catch's payload into the target's slots and branches to it.
	 * \param target Control frame the catch label names.
	 * \param payload The caught tag's payload signature.
	 * \param carryExn True for `catch_ref`/`catch_all_ref`, which also carry the `exnref`.
	 */
	void emitCatchBranch(ControlFrame& target, const WASM::FuncType& payload, bool carryExn);
	//! Emits a catch that targets the function label by returning its payload.
	//! \param payload Tag payload type.
	//! \param carryExn True for `catch_ref`/`catch_all_ref` (trailing exnref).
	void emitCatchReturn(const WASM::FuncType& payload, bool carryExn);
	//! Returns the zero of each declared result without annotating a trap. Used
	//! when an uncaught exception leaves the function: the caller sees the
	//! pending slot before any value.
	void emitZeroReturn();
	/*!
	 * \brief Byte offset of the reserved slot holding the exception being routed.
	 * \return Offset from `SLJIT_SP`.
	 */
	sljit_sw exceptionPtrSlotOffset() const;
	//! Emits an unconditional branch to the trap epilogue.
	void emitTrap(WASM::TrapKind kind);
	/** \brief Emits an internal trap carrying a detail string. \param detail Interned detail text. */
	void emitTrapDetail(const char* detail);
	/** \brief Releases continuation handles held by reference locals before a return. */
	void emitDropReferenceLocals();
	void emitTrapBranch();
	/*!
	 * \brief Emits a two-operand integer operation over the top two slots.
	 * \param wordOp Word-width SLJIT operation; the 32-bit form is derived.
	 * \param width 4 for i32, 8 for i64.
	 */
	void emitIntegerBinary(sljit_s32 wordOp, int width);
	/*!
	 * \brief Emits a one-operand integer operation over the top slot.
	 * \param wordOp Word-width SLJIT operation; the 32-bit form is derived.
	 * \param width 4 for i32, 8 for i64.
	 * \param resultWidth Width of the result slot.
	 */
	void emitIntegerUnary(sljit_s32 wordOp, int width, int resultWidth);
	/*!
	 * \brief Emits a comparison of the top two slots, producing an i32 0/1.
	 * \param condition SLJIT comparison for "true".
	 * \param width Operand width (4 or 8).
	 */
	void emitCompare(sljit_s32 condition, int width);
	/*!
	 * \brief Emits `eqz` over the top slot, producing an i32 0/1.
	 * \param width Operand width (4 or 8).
	 */
	void emitEqz(int width);
	/*!
	 * \brief Emits a checked division or remainder through a C helper.
	 * \param address Helper entry point.
	 * \param width 4 for i32, 8 for i64.
	 * \param operation Human-readable opcode name for diagnostics.
	 */
	void emitCheckedBinary(sljit_sw address, int width, const char* operation);
	/*!
	 * \brief Loads one operand-stack float slot into an `FR` register.
	 * \param freg Destination floating-point register.
	 * \param depth Zero-based operand-stack depth.
	 * \param width 4 for f32, 8 for f64.
	 */
	void loadFloatSlot(int freg, std::size_t depth, int width);
	/*!
	 * \brief Stores an `FR` register into one operand-stack float slot.
	 * \param freg Source floating-point register.
	 * \param depth Zero-based operand-stack depth.
	 * \param width 4 for f32, 8 for f64.
	 */
	void storeFloatSlot(int freg, std::size_t depth, int width);
	/*!
	 * \brief Replaces a NaN in one operand-stack slot with the canonical pattern.
	 *
	 * A no-op unless the deterministic profile is active. The slot is treated as
	 * raw IEEE-754 bits, never loaded through an `FR` register: a floating-point
	 * move may itself quiet or re-sign a signalling NaN, which would make the
	 * canonicalisation depend on the host instruction set
	 * (docs/proposals/PROFILES.md PR-3).
	 *
	 * \param depth Zero-based operand-stack depth.
	 * \param width 4 for f32, 8 for f64.
	 */
	void canonicalizeSlot(std::size_t depth, int width);
	/*!
	 * \brief Emits a one-operand float operation over the top slot.
	 * \param wordOp Word-width SLJIT FOP1 operation; the 32-bit form is derived.
	 * \param width 4 for f32, 8 for f64.
	 */
	void emitFloatUnary(sljit_s32 wordOp, int width);
	/*!
	 * \brief Emits a two-operand float operation over the top two slots.
	 * \param wordOp Word-width SLJIT FOP2 operation; the 32-bit form is derived.
	 * \param width 4 for f32, 8 for f64.
	 */
	void emitFloatBinary(sljit_s32 wordOp, int width);
	/*!
	 * \brief Calls a one-argument float helper over the top slot.
	 * \param address Helper entry point.
	 * \param width 4 for f32, 8 for f64.
	 */
	void emitFloatHelper1(sljit_sw address, int width);
	/*!
	 * \brief Calls a two-argument float helper over the top two slots.
	 * \param address Helper entry point.
	 * \param width 4 for f32, 8 for f64.
	 */
	void emitFloatHelper2(sljit_sw address, int width);
	/*!
	 * \brief Emits a float comparison of the top two slots, producing an i32 0/1.
	 * \param condition SLJIT comparison for "true".
	 * \param width 4 for f32, 8 for f64.
	 */
	void emitFloatCompare(sljit_s32 condition, int width);
	/*!
	 * \brief Writes a float constant into one operand-stack slot.
	 * \param bits The IEEE bit pattern.
	 * \param width 4 for f32, 8 for f64.
	 * \param depth Zero-based operand-stack depth.
	 */
	void emitFloatConst(std::uint64_t bits, int width, std::size_t depth);
	/*!
	 * \brief Emits one checked float memory load.
	 * \param arg Memory immediate.
	 * \param width 4 for f32, 8 for f64.
	 */
	void emitMemoryLoadFloat(const WASM::MemArg& arg, int width);
	/*!
	 * \brief Emits one checked float memory store.
	 * \param arg Memory immediate.
	 * \param width 4 for f32, 8 for f64.
	 */
	void emitMemoryStoreFloat(const WASM::MemArg& arg, int width);
	/*!
	 * \brief Emits an integer-to-float conversion over the top slot.
	 * \param conversion SLJIT FOP1 conversion opcode.
	 * \param sourceWidth Width of the integer operand (4 or 8).
	 * \param resultWidth Width of the float result (4 or 8).
	 */
	void emitIntToFloat(sljit_s32 conversion, int sourceWidth, int resultWidth);
	/*!
	 * \brief Emits a checked float-to-integer conversion over the top slot.
	 * \param address Checked helper entry point.
	 * \param sourceWidth Width of the float operand (4 or 8).
	 * \param resultWidth Width of the integer result (4 or 8).
	 */
	void emitFloatToInt(sljit_sw address, int sourceWidth, int resultWidth);
	/*!
	 * \brief Emits a bit-reinterpretation of the top slot.
	 * \param width Slot width in bytes (4 or 8); both reinterpreting types
	 *        always have the same width.
	 * \param canonicalizeFloat True for a float-to-integer reinterpret, whose
	 *        payload becomes host-observable under the deterministic profile.
	 */
	void emitReinterpret(int width, bool canonicalizeFloat);

	/*!
	 * \brief Resolves a block type into parameter and result widths.
	 * \param type Block type to resolve.
	 * \return The parameter and result widths.
	 */
	BlockSignature blockSignature(const WASM::BlockType& type);
	/*!
	 * \brief Emits the branch-transfer and jump to one structured frame.
	 * \param target The frame to branch to.
	 */
	void emitBranchTo(ControlFrame& target);

	/*!
	 * \brief Emits `call`, or its tail-call form.
	 * \param index Function index.
	 * \param isTail True when the caller must return the callee's result.
	 */
	void emitCall(WASM::FuncIdx index, bool isTail);
	/*!
	 * \brief Emits `call_indirect`, or its tail-call form.
	 * \param typeIndex Type index the callee must match.
	 * \param tableIndex Table index.
	 * \param isTail True when the caller must return the callee's result.
	 */
	void emitCallIndirect(WASM::TypeIdx typeIndex, WASM::TableIdx tableIndex, bool isTail);
	/*!
	 * \brief Emits the post-resolver half of a call: context selection, argument
	 *        registers, the indirect call and the result store.
	 * \param type Callee signature.
	 * \param callableReg Register holding the resolved `Callable*`.
	 */
	void emitResolvedCall(const WASM::FuncType& type, int callableReg);

	/*!
	 * \brief Validates a memory index and returns its type.
	 * \param index Memory index.
	 * \return The declared memory type.
	 */
	const WASM::MemoryType& requireMemoryIndex(WASM::MemIdx index);
	/*!
	 * \brief Emits the checked effective address of a memory access into R0.
	 *
	 * The helper follows the memory's index type: memory32 packs the immediate
	 * offset with the access width, memory64 passes the 64-bit address and
	 * offset separately. A pending trap branches to the epilogue, so the caller
	 * must use R0 only when execution continues.
	 *
	 * \param arg Memory immediate.
	 * \param width Access width in bytes.
	 * \param addressDepth Operand-stack depth holding the address.
	 */
	void emitCheckedMemoryAddress(const WASM::MemArg& arg, std::uint32_t width,
			std::size_t addressDepth);
	/*!
	 * \brief Loads the address of one operand-stack slot into a register.
	 * \param reg Destination register.
	 * \param depth Zero-based operand-stack depth.
	 */
	void slotAddress(int reg, std::size_t depth);
	/*!
	 * \brief Copies 16 bytes from a local slot to an operand-stack slot.
	 * \param localIndex Local index.
	 * \param depth Operand-stack destination.
	 */
	void copyLocalToStack(std::size_t localIndex, std::size_t depth);
	/*!
	 * \brief Copies 16 bytes from an operand-stack slot to a local slot.
	 * \param depth Operand-stack source.
	 * \param localIndex Local index.
	 */
	void copyStackToLocal(std::size_t depth, std::size_t localIndex);
	/*!
	 * \brief Calls a unary v128 helper over the top slot.
	 * \param address Helper entry point.
	 */
	void emitV128Unary(sljit_sw address);
	//! Emits `i31.get_s` (arithmetic) or `i31.get_u` (logical) over the top slot.
	//! \param signExtend True for the signed extraction.
	void emitI31Get(bool signExtend);
	//! Emits an extern/any bridge helper over the top reference slot.
	//! \param address Address of the C bridge helper.
	void emitExternConversion(sljit_sw address);
	/*!
	 * \brief Calls a v128 reduction helper and leaves its i32 result.
	 * \param address Helper entry point.
	 */
	void emitV128Reduce(sljit_sw address);
	/*!
	 * \brief Calls a shift helper over the top vector and its scalar count.
	 * \param address Helper entry point.
	 */
	void emitV128Shift(sljit_sw address);
	/*!
	 * \brief Calls a float splat helper over the top scalar slot.
	 * \param address Helper entry point.
	 * \param width Scalar width in bytes (4 or 8).
	 */
	void emitV128SplatFloat(sljit_sw address, int width);
	void emitV128ExtractInteger(sljit_sw address, int resultWidth, uint8_t lane);
	void emitV128ReplaceInteger(sljit_sw address, int scalarWidth, uint8_t lane);
	void emitV128SplatInteger(sljit_sw address, int scalarWidth);
	/*!
	 * \brief Calls a float extract helper and leaves the lane as a scalar.
	 * \param address Helper entry point.
	 * \param width Scalar width in bytes (4 or 8).
	 * \param lane Lane index.
	 */
	void emitV128ExtractFloat(sljit_sw address, int width, uint8_t lane);
	/*!
	 * \brief Calls a float replace helper over a vector and a scalar lane.
	 * \param address Helper entry point.
	 * \param width Scalar width in bytes (4 or 8).
	 * \param lane Lane index.
	 */
	void emitV128ReplaceFloat(sljit_sw address, int width, uint8_t lane);
	/*!
	 * \brief Calls a load-transform helper (extending, splatting or zeroing).
	 * \param arg Memory immediate.
	 * \param width Bytes read from memory.
	 * \param address Helper entry point.
	 */
	void emitV128LoadTransform(const WASM::MemArg& arg, std::uint32_t width, sljit_sw address);
	/*!
	 * \brief Calls a lane-load helper over the address and the vector below it.
	 * \param arg Memory immediate.
	 * \param width Lane width in bytes.
	 * \param lane Lane index.
	 * \param address Helper entry point.
	 */
	void emitV128LoadLane(const WASM::MemArg& arg, std::uint32_t width, uint8_t lane, sljit_sw address);
	/*!
	 * \brief Calls a lane-store helper over the address and the vector below it.
	 * \param arg Memory immediate.
	 * \param width Lane width in bytes.
	 * \param lane Lane index.
	 * \param address Helper entry point.
	 */
	void emitV128StoreLane(const WASM::MemArg& arg, std::uint32_t width, uint8_t lane, sljit_sw address);
	/*!
	 * \brief Calls the shuffle helper over the top two vectors.
	 * \param selectors The 16 shuffle indices.
	 * \param address Helper entry point.
	 */
	void emitV128Shuffle(std::span<uint8_t> selectors, sljit_sw address);
	/*!
	 * \brief Calls a binary v128 helper over the top two slots.
	 * \param address Helper entry point.
	 */
	void emitV128Binary(sljit_sw address);
	//! Emits a ternary v128 helper (`a`, `b`, `c`, `out`) over the top three slots.
	void emitV128Ternary(sljit_sw address);
	/*!
	 * \brief Emits a reference cast: null handling plus the type-id check.
	 * \param arg Target heap type (must be a concrete type index).
	 * \param nullable Whether null passes through instead of trapping.
	 */
	void emitRefCast(const WASM::HeapType& arg, bool nullable);
	//! Emits a `ref.test` predicate through the core heap-type matcher.
	//! \param arg Heap type to test.
	//! \param nullable True for the nullable form.
	void emitRefTest(const WASM::HeapType& arg, bool nullable);
	//! Emits the shared `br_on_cast`/`br_on_cast_fail` predicate and branch.
	//! \param castop Instruction flags (bit 1 = nullable target).
	//! \param depth Target label depth.
	//! \param target Target heap type.
	//! \param onFailure True for `br_on_cast_fail`.
	void emitBrOnCast(uint8_t castop, WASM::LabelIdx depth,
			const WASM::HeapType& target, bool onFailure);
	/*!
	 * \brief Emits a sign- or zero-extending read of a packed struct field.
	 * \param arg1 Struct type index.
	 * \param arg2 Field index.
	 * \param signExtend Whether the field is sign-extended.
	 */
	void emitStructGetPacked(WASM::TypeIdx arg1, uint32_t arg2, bool signExtend);
	/*!
	 * \brief Emits a sign- or zero-extending read of a packed array element.
	 * \param arg Array type index.
	 * \param signExtend Whether the element is sign-extended.
	 */
	void emitArrayGetPacked(WASM::TypeIdx arg, bool signExtend);
	/*!
	 * \brief Emits one checked memory load.
	 * \param arg Memory immediate.
	 * \param width Access width in bytes.
	 * \param loadOp SLJIT load operation for the access.
	 * \param resultWidth Width of the value pushed on the operand stack.
	 */
	void emitMemoryLoad(const WASM::MemArg& arg, std::uint32_t width,
			sljit_s32 loadOp, int resultWidth);
	/*!
	 * \brief Emits one checked memory store.
	 * \param arg Memory immediate.
	 * \param width Access width in bytes.
	 * \param storeOp SLJIT store operation for the access.
	 */
	void emitMemoryStore(const WASM::MemArg& arg, std::uint32_t width,
			sljit_s32 storeOp);
	/*!
	 * \brief Emits one atomic load through the shared alignment-checked helper.
	 * \param arg Memory immediate, including the ordering byte.
	 * \param width Access width in bytes.
	 * \param resultWidth Width of the value pushed (4 for i32, 8 for i64).
	 */
	void emitAtomicLoad(const WASM::MemArg& arg, std::uint32_t width, int resultWidth);
	/*!
	 * \brief Emits one atomic store through the shared alignment-checked helper.
	 * \param arg Memory immediate, including the ordering byte.
	 * \param width Access width in bytes.
	 * \param valueWidth Width of the stored operand (4 for i32, 8 for i64).
	 */
	void emitAtomicStore(const WASM::MemArg& arg, std::uint32_t width, int valueWidth);
	/*!
	 * \brief Emits one atomic read-modify-write through the shared helper.
	 * \param arg Memory immediate, including the ordering byte.
	 * \param width Access width in bytes.
	 * \param op The `WASM_ATOMIC_*` operation selector.
	 * \param valueWidth Width of the operand (4 or 8).
	 * \param resultWidth Width of the old value pushed (4 or 8).
	 */
	void emitAtomicRmw(const WASM::MemArg& arg, std::uint32_t width, std::uint32_t op,
			int valueWidth, int resultWidth);
	/*!
	 * \brief Emits one atomic compare-exchange through the shared helper.
	 * \param arg Memory immediate, including the ordering byte.
	 * \param width Access width in bytes.
	 * \param valueWidth Width of the expected/replacement operands (4 or 8).
	 * \param resultWidth Width of the observed value pushed (4 or 8).
	 */
	void emitAtomicCmpxchg(const WASM::MemArg& arg, std::uint32_t width, int valueWidth,
			int resultWidth);
	/*!
	 * \brief Emits `memory.atomic.wait32/64` through the shared wait helper.
	 * \param arg Memory immediate.
	 * \param width Access width in bytes (4 or 8).
	 * \param valueWidth Width of the expected operand (4 or 8).
	 */
	void emitAtomicWait(const WASM::MemArg& arg, std::uint32_t width, int valueWidth);
	/*!
	 * \brief Loads a global's Value cell address into R0.
	 * \param arg Global index.
	 */
	void emitGlobalAtomicAddress(WASM::GlobalIdx arg);
	/*!
	 * \brief Loads a struct field's address into R0, trapping on null.
	 * \param refDepth Operand-stack depth of the struct reference.
	 * \param fieldOffset Byte offset of the field in the object.
	 */
	void emitStructAtomicAddress(std::size_t refDepth, std::size_t fieldOffset);
	/*!
	 * \brief Loads an array element's address into R0, trapping on null/bounds.
	 * \param refDepth Operand-stack depth of the array reference.
	 * \param indexDepth Operand-stack depth of the index.
	 * \param lengthOffset Byte offset of the length word in the header.
	 * \param dataOffset Byte offset of element 0.
	 * \param elementBytes Size of one element in bytes.
	 */
	void emitArrayAtomicAddress(std::size_t refDepth, std::size_t indexDepth,
			std::size_t lengthOffset, std::size_t dataOffset, std::uint32_t elementBytes);
	/*!
	 * \brief Calls the atomic-load helper with the address in R0.
	 * \param ordering Acquire-release ordering byte.
	 * \param width Access width in bytes.
	 * \param resultWidth Width of the result slot.
	 * \param depth Operand-stack depth receiving the result.
	 * \param signBits Sign-extend from this bit width (8 or 16), or 0 to leave the
	 *        helper's zero extension in place.
	 */
	void emitSharedAtomicLoad(uint8_t ordering, std::uint32_t width, int resultWidth,
			std::size_t depth, int signBits = 0);
	/*!
	 * \brief Calls the atomic-store helper with the address in R0.
	 * \param ordering Acquire-release ordering byte.
	 * \param width Access width in bytes.
	 * \param valueWidth Width of the stored operand.
	 * \param valueDepth Operand-stack depth of the value.
	 */
	void emitSharedAtomicStore(uint8_t ordering, std::uint32_t width, int valueWidth,
			std::size_t valueDepth);
	/*!
	 * \brief Calls the atomic RMW helper with the address in R0.
	 * \param ordering Acquire-release ordering byte.
	 * \param width Access width in bytes.
	 * \param op The `WASM_ATOMIC_*` selector.
	 * \param valueWidth Width of the operand and the result.
	 * \param valueDepth Operand-stack depth of the operand.
	 * \param resultDepth Operand-stack depth receiving the old value.
	 */
	void emitSharedAtomicRmw(uint8_t ordering, std::uint32_t width, std::uint32_t op,
			int valueWidth, std::size_t valueDepth, std::size_t resultDepth);
	/*!
	 * \brief Calls the atomic compare-exchange helper with the address in R0.
	 * \param ordering Acquire-release ordering byte.
	 * \param width Access width in bytes.
	 * \param valueWidth Width of the operands and the result.
	 * \param expectedDepth Operand-stack depth of the expected value.
	 * \param replacementDepth Operand-stack depth of the replacement.
	 * \param resultDepth Operand-stack depth receiving the observed value.
	 */
	void emitSharedAtomicCmpxchg(uint8_t ordering, std::uint32_t width, int valueWidth,
			std::size_t expectedDepth, std::size_t replacementDepth, std::size_t resultDepth);
	/*!
	 * \brief Validates a global index and returns its type.
	 * \param index Global index.
	 * \return The declared global type.
	 */
	const WASM::GlobalType& requireGlobalIndex(WASM::GlobalIdx index);
	/*!
	 * \brief Validates a table index.
	 * \param index Table index.
	 */
	void requireTableIndex(WASM::TableIdx index);

	void dispatchStringConst(std::uint32_t idx) override; //!< `string.const`.
	void dispatchStringMeasureUtf8() override; //!< `string.measure_utf8`.
	void dispatchStringMeasureWtf8() override; //!< `string.measure_wtf8`.
	void dispatchStringMeasureWtf16() override; //!< `string.measure_wtf16`.
	void dispatchStringConcat() override; //!< `string.concat`.
	void dispatchStringEq() override; //!< `string.eq`.
	void dispatchStringIsUsvSequence() override; //!< `string.is_usv_sequence`.
	void dispatchI32Itt(std::uint32_t immediate) override; //!< `i32.itt` compile-time marker.
	void dispatchRoundingVariant(std::uint32_t sub) override; //!< Rounding-mode variants.
	void dispatchMemoryDiscard(WASM::MemIdx memory) override; //!< `memory.discard`.
	void dispatchStringNewUtf8(WASM::MemIdx memory) override; //!< `string.new_utf8`.
	void dispatchStringNewLossyUtf8(WASM::MemIdx memory) override; //!< `string.new_lossy_utf8`.
	void dispatchStringNewWtf8(WASM::MemIdx memory) override; //!< `string.new_wtf8`.
	void dispatchStringNewWtf16(WASM::MemIdx memory) override; //!< `string.new_wtf16`.
	void dispatchStringEncodeUtf8(WASM::MemIdx memory) override; //!< `string.encode_utf8`.
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx memory) override; //!< `string.encode_lossy_utf8`.
	void dispatchStringEncodeWtf8(WASM::MemIdx memory) override; //!< `string.encode_wtf8`.
	void dispatchStringEncodeWtf16(WASM::MemIdx memory) override; //!< `string.encode_wtf16`.
	void dispatchStringNewUtf8Array(WASM::TypeIdx type) override; //!< `string.new_utf8_array`.
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx type) override; //!< `string.new_lossy_utf8_array`.
	void dispatchStringNewWtf8Array(WASM::TypeIdx type) override; //!< `string.new_wtf8_array`.
	void dispatchStringNewWtf16Array(WASM::TypeIdx type) override; //!< `string.new_wtf16_array`.
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx type) override; //!< `string.encode_utf8_array`.
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx type) override; //!< `string.encode_lossy_utf8_array`.
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx type) override; //!< `string.encode_wtf8_array`.
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx type) override; //!< `string.encode_wtf16_array`.
	void dispatchStringAsWtf8() override; //!< `string.as_wtf8`.
	void dispatchStringViewWtf8Advance() override; //!< `stringview_wtf8.advance`.
	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx memory) override; //!< `stringview_wtf8.encode_utf8`.
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx memory) override; //!< `stringview_wtf8.encode_lossy_utf8`.
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx memory) override; //!< `stringview_wtf8.encode_wtf8`.
	void dispatchStringViewWtf8Slice() override; //!< `stringview_wtf8.slice`.
	void dispatchStringAsWtf16() override; //!< `string.as_wtf16`.
	void dispatchStringViewWtf16Length() override; //!< `stringview_wtf16.length`.
	void dispatchStringViewWtf16GetCodeUnit() override; //!< `stringview_wtf16.get_codeunit`.
	void dispatchStringViewWtf16Encode(WASM::MemIdx memory) override; //!< `stringview_wtf16.encode`.
	void dispatchStringViewWtf16Slice() override; //!< `stringview_wtf16.slice`.
	void dispatchStringAsIter() override; //!< `string.as_iter`.
	void dispatchStringViewIterNext() override; //!< `stringview_iter.next`.
	void dispatchStringViewIterAdvance() override; //!< `stringview_iter.advance`.
	void dispatchStringViewIterRewind() override; //!< `stringview_iter.rewind`.
	void dispatchStringViewIterSlice() override; //!< `stringview_iter.slice`.
	void dispatchContNew(WASM::TypeIdx type) override; //!< `cont.new`.
	void dispatchContBind(WASM::TypeIdx from, WASM::TypeIdx to) override; //!< `cont.bind`.
	void dispatchSuspend(WASM::TagIdx tag) override; //!< `suspend`.
	void dispatchSwitch(WASM::TypeIdx type, WASM::TagIdx tag) override; //!< `switch`.
	void dispatchResume(WASM::TypeIdx type, std::vector<WASM::ContHandlerClause>&& handlers) override; //!< `resume`.
	void dispatchResumeThrow(WASM::TypeIdx type, WASM::TagIdx tag, std::vector<WASM::ContHandlerClause>&& handlers) override; //!< `resume_throw`.
	void dispatchResumeThrowRef(WASM::TypeIdx type, std::vector<WASM::ContHandlerClause>&& handlers) override; //!< `resume_throw_ref`.

private:
	void emitStringViewCreate(const void* helper); //!< Shared view/iterator creation lowering.
	void emitStringViewWtf8Encode(const void* helper, WASM::MemIdx memory); //!< Shared WTF-8 view encoder lowering.
	void emitStringViewRange(const void* helper); //!< Shared context/view/range slice lowering.
	void emitStringViewWtf16Encode(WASM::MemIdx memory); //!< WTF-16 view encoder lowering.

public:

	void emitStringNew(WASM::MemIdx memory, const void* helper);
	void emitStringEncode(WASM::MemIdx memory, const void* helper);
	void emitStringNewArray(WASM::TypeIdx type, int mode);
	void emitStringEncodeArray(WASM::TypeIdx type, int mode);

	const WASM::Module& module_; //!< Module owning the body; borrowed.
	const WASM::FuncType& funcType_; //!< Declared signature of the function.
	const WASM::FunctionBody& body_; //!< Body being lowered.
	const std::vector<WASM::TagId>& tagIds_; //!< Instance tag index space, imports first.
	std::uint32_t functionIndex_; //!< Module function index (trap-site annotation).
	WASM::ModuleInstanceInternals& internals_; //!< Instance callables for direct tail targets.
	bool tailTrampoline_ = false; //!< True when this body may write TailCallState.
	const char* functionName_ = nullptr; //!< Name-section name, or null.

	std::vector<int> localWidths_; //!< Slot width per local, parameters first.
	std::vector<WASM::ValueTypeCode> localKinds_; //!< Value kind per local, parameters first.
	std::vector<WASM::ValueTypeCode> paramKinds_; //!< Value kind of each parameter.
	std::vector<int> paramWidths_; //!< Slot width of each parameter.
	std::uint32_t parameterCount_ = 0; //!< Number of Wasm parameters.
	std::size_t localSlots_ = 0; //!< Parameters plus declared locals.
	int resultWidth_ = 0; //!< 0 (void), 4 (i32/f32) or 8 (i64/f64).
	WASM::ValueTypeCode resultKind_ = WASM::ValueTypeCode::Void; //!< Declared result kind.
	//! Every result's kind and byte width, in declaration order. Populated whether or
	//! not multiple results are supported, so the frame-form return site can use one
	//! loop for one result or many (docs/backends/EXPORT_SIGNATURE.md section 40).
	std::vector<WASM::ValueTypeCode> resultKinds_;
	std::vector<int> resultWidths_;

	std::vector<int> stackWidths_; //!< Widths of the live operand stack, bottom first.
	//! String-family ownership bits for the same stack; 1 means the slot owns one
	//! `StringStore` reference and must release it on `drop`.
	std::vector<std::uint8_t> stackRefKinds_;
	std::size_t maxStackDepth_ = 0; //!< High-water mark of the operand stack.
	//! Set during measurement when the body uses a frame-form call.
	mutable bool needsStagingCall_ = false;
	//! Set during measurement when the body uses a continuation operation.
	mutable bool needsContinuationFrame_ = false;
	//! Operand depth the prologue reserved on the emitting pass. A push past it
	//! is a compiler bug and is refused before SLJIT can assert.
	std::size_t frameStackCapacity_ = 0;
	std::vector<ControlFrame> controls_; //!< Active structured-control frames.
	std::vector<sljit_jump*> trapJumps_; //!< Branches to the trap epilogue.
	//! Body-entry label, the target of a direct self tail call.
	sljit_label* bodyStart_ = nullptr;

	sljit_compiler* compiler_ = nullptr; //!< Compiler for the emitting pass.
	bool ok_ = true; //!< False once any SLJIT call has failed.
	bool supported_ = false; //!< True when the signature is inside the subset.
	bool measureOnly_ = true; //!< True while measuring the operand-stack depth.
	bool reachable_ = true; //!< True while the current instruction can execute.
	std::string unsupportedReason_; //!< Why the body was refused; empty on success.
};

} // namespace Sljit

#endif // SLJITOPCODEDISPATCHER_HPP




