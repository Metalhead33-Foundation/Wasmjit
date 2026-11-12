#ifndef WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP
#define WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP

#include "../WasmBase/WasmType.hpp"
#include "../WasmBase/WasmBackendAbi.hpp"
#include <asmjit/core.h>

/*!
  \file AsmjitTypeTranslator.hpp
  \brief Maps Wasm value and storage types to AsmJit types.

  This is an internal backend component, not an embedder-facing API.
*/

namespace WASM { class Module; }

namespace AsmJitBackend {

/*!
 * \brief Builds AsmJit host-ABI signatures for Wasm function types.
 *
 * The engine's native function ABI prepends VMContext* to every function.
 * Multi-value results use a trailing void* result-buffer argument and a void
 * return, matching docs/proposals/MULTI_VALUE.md MV5. References are opaque
 * pointers at this ABI boundary. V128 maps to AsmJit's 128-bit vector TypeId,
 * allowing FuncDetail to apply the host's native by-value vector convention.
 *
 * Packed i8/i16 are supported as storage types. They retain their 8-/16-bit
 * representation in storage; generated arithmetic must explicitly extend
 * them to i32 before performing Wasm integer operations.
 */
class TypeTranslator
{
public:
	/*!
	 * \brief Maps one Wasm value type to an AsmJit TypeId.
	 * \param type Wasm value type.
	 * \return Native AsmJit type identifier.
	 * \throws std::runtime_error for unsupported value encodings.
	 */
	static asmjit::TypeId translateValueType(const WASM::ValueType& type);

	/*!
	 * \brief Maps a Wasm storage type, including packed i8/i16.
	 * \param type Wasm storage type.
	 * \return Native AsmJit type identifier preserving the storage width.
	 * \throws std::runtime_error for unsupported storage encodings.
	 */
	static asmjit::TypeId translateStorageType(const WASM::StorageType& type);

	/*!
	 * \brief Returns this backend's register-form limits.
	 * \return The four-entry ceiling; a `v128` or multi-result signature takes the frame.
	 */
	static WASM::RegisterFormCeiling registerFormCeiling() noexcept;
	/*!
	 * \brief Decides the call form per module-local type index.
	 * \param module Module whose function types are examined.
	 * \param optIn Whether the process-wide frame opt-in is set.
	 * \return One entry per module type: non-zero for the staging frame.
	 */
	static std::vector<std::uint8_t> frameForms(const WASM::Module& module, bool optIn);
	/*!
	 * \brief Reports whether every defined function uses the frame form.
	 * \param module Module whose internal function types are examined.
	 * \param forms The per-type table from `frameForms`.
	 * \return True when `ModuleInstance::frameCallAbi()` should report the uniform form.
	 */
	static bool uniformlyFrameForm(const WASM::Module& module,
			const std::vector<std::uint8_t>& forms);

/*!
	 * \brief Builds the native function signature used by generated code.
	 * \param function Wasm function type.
	 * \param frameAbi Whether to spell the staging-frame form, `(ctx, frame) -> void`.
	 * \return C-declaration host signature, including the implicit VMContext*.
	 * \throws std::runtime_error for unsupported types or too many arguments.
	 */
		static asmjit::FuncSignature translateFunctionType(const WASM::FuncType& function,
			bool frameAbi = false);

};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP
