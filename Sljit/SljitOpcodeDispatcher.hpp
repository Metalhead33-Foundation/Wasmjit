#ifndef SLJITOPCODEDISPATCHER_HPP
#define SLJITOPCODEDISPATCHER_HPP

#include "../WasmBase/WasmModuleInstance.hpp"
#include "../WasmBase/WasmOpcodeDispatcher.hpp"

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

/*!
 * \brief Compiles one WebAssembly function body into SLJIT machine code.
 *
 * The emitter is deliberately a *presence* implementation, not a complete
 * backend: it lowers a straight-line integer subset and refuses everything else
 * by leaving the function without an entry point, so the core refuses the call
 * at the boundary instead of executing a half-emitted function
 * (docs/WRITING_A_BACKEND.md §9).
 *
 * Frame model: every Wasm local and every operand-stack slot is one machine
 * word in the function's local stack area, addressed as `SLJIT_MEM1(SLJIT_SP)`.
 * Two scratch registers (R0, R1) carry every operation, so register pressure is
 * constant and independent of the body. Parameters arrive in the saved
 * registers SLJIT assigns (`VMContext*` in S0, parameter *i* in S(i+1)) and are
 * copied into their frame slots in the prologue.
 *
 * A function is compiled in two passes over the decoded body: the first
 * measures the operand-stack high-water mark (the frame cannot grow after
 * `sljit_emit_enter`), the second emits. Both passes share the same visit code,
 * so the measured depth cannot disagree with the emitted one.
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
	 */
	OpcodeDispatcher(const WASM::Module& module, const WASM::FuncType& funcType,
			const WASM::FunctionBody& body, std::uint32_t functionIndex);

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
	void dispatchEnd() override; //!< The function's terminating `end` (implicit return).
	void dispatchReturn() override; //!< `return`: an explicit function exit.
	void dispatchDrop() override; //!< `drop`: pop one operand-stack slot.
	/*!
	 * \brief `local.get`: copy a local slot onto the operand stack.
	 * \param arg Local index.
	 */
	void dispatchLocalGet(WASM::LocalIdx arg) override;
	/*!
	 * \brief `local.set`: pop the operand stack into a local slot.
	 * \param arg Local index.
	 */
	void dispatchLocalSet(WASM::LocalIdx arg) override;
	/*!
	 * \brief `local.tee`: copy the operand stack top into a local, keeping it.
	 * \param arg Local index.
	 */
	void dispatchLocalTee(WASM::LocalIdx arg) override;
	/*!
	 * \brief `i32.const`: push a 32-bit immediate.
	 * \param arg The constant.
	 */
	void dispatchI32Const(std::int32_t arg) override;
	/*!
	 * \brief `i64.const`: push a 64-bit immediate.
	 * \param arg The constant.
	 */
	void dispatchI64Const(std::int64_t arg) override;
	void dispatchI32Add() override; //!< `i32.add`.
	void dispatchI32Sub() override; //!< `i32.sub`.
	void dispatchI32Mul() override; //!< `i32.mul`.
	void dispatchI32And() override; //!< `i32.and`.
	void dispatchI32Or() override; //!< `i32.or`.
	void dispatchI32Xor() override; //!< `i32.xor`.
	void dispatchI32Shl() override; //!< `i32.shl`.
	void dispatchI32ShrS() override; //!< `i32.shr_s`.
	void dispatchI32ShrU() override; //!< `i32.shr_u`.
	void dispatchI64Add() override; //!< `i64.add`.
	void dispatchI64Sub() override; //!< `i64.sub`.
	void dispatchI64Mul() override; //!< `i64.mul`.
	void dispatchI64And() override; //!< `i64.and`.
	void dispatchI64Or() override; //!< `i64.or`.
	void dispatchI64Xor() override; //!< `i64.xor`.
	void dispatchI64Shl() override; //!< `i64.shl`.
	void dispatchI64ShrS() override; //!< `i64.shr_s`.
	void dispatchI64ShrU() override; //!< `i64.shr_u`.

private:
	/*!
	 * \brief Runs one pass over the body.
	 * \param measure true for the depth-measuring pass (no code is emitted).
	 * \return true when the whole body was visited and lowered.
	 */
	bool run(bool measure);
	/*!
	 * \brief Emits the prologue: frame, stack guard, pending check, argument copy.
	 * \return true when the prologue was emitted successfully.
	 */
	bool beginFunction();
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
	//! Pushes one operand-stack slot.
	void push();
	/*!
	 * \brief Refuses the function unless at least `count` operands are present.
	 * \param count Required operand-stack depth.
	 */
	void requireStack(std::size_t count);
	/*!
	 * \brief Refuses the function unless `condition` holds.
	 * \param condition Tested condition.
	 * \param reason Message recorded when the condition fails.
	 */
	void require(bool condition, const char* reason);
	/*!
	 * \brief Refuses a local index outside the frame.
	 * \param index Local index to check.
	 * \param what Opcode name for the diagnostic.
	 */
	void requireLocal(std::size_t index, const char* what);
	/*!
	 * \brief Refuses any instruction after an explicit return.
	 * \param what Opcode name for the diagnostic.
	 */
	void guardNotReturned(const char* what);
	/*!
	 * \brief Lowers one integer binary operator over two operand-stack slots.
	 * \param wordOp Word-width SLJIT operation; the 32-bit form is derived.
	 * \param width 4 for i32, 8 for i64.
	 */
	void emitIntegerBinary(sljit_s32 wordOp, int width);
	//! Emits the return of the function's result (peeking at the stack top).
	void emitReturnValue();
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

	const WASM::Module& module_; //!< Module owning the body; borrowed.
	const WASM::FuncType& funcType_; //!< Declared signature of the function.
	const WASM::FunctionBody& body_; //!< Body being lowered.
	std::uint32_t functionIndex_; //!< Module function index (trap-site annotation).
	const char* functionName_ = nullptr; //!< Name-section name, or null.

	std::vector<int> localWidths_; //!< 4 (i32) or 8 (i64) per local, parameters first.
	std::vector<int> paramWidths_; //!< Width of each parameter, in order.
	std::uint32_t parameterCount_ = 0; //!< Number of Wasm parameters.
	std::size_t localSlots_ = 0; //!< Parameters plus declared locals.
	int resultWidth_ = 0; //!< 0 (void), 4 (i32) or 8 (i64).

	sljit_compiler* compiler_ = nullptr; //!< Compiler for the emitting pass.
	sljit_jump* trapJump_ = nullptr; //!< Branch to the trap epilogue.
	bool ok_ = true; //!< False once any SLJIT call has failed.
	bool supported_ = false; //!< True when the signature is inside the subset.
	bool measureOnly_ = true; //!< True while measuring the operand-stack depth.
	bool returned_ = false; //!< True once the function has returned.
	std::size_t stackDepth_ = 0; //!< Current operand-stack depth.
	std::size_t maxStackDepth_ = 0; //!< High-water mark of the operand stack.
	std::string unsupportedReason_; //!< Why the body was refused; empty on success.
};

} // namespace Sljit

#endif // SLJITOPCODEDISPATCHER_HPP
