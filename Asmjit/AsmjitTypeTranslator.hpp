#ifndef WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP
#define WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP

#include "../WasmBase/WasmType.hpp"
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
	 * \brief Reports whether a signature cannot be delivered by the register form.
	 * \param function Wasm function type.
	 * \return True when the signature needs the staging frame: a `v128` anywhere,
	 *         more than one result, or more argument entries than the C ABI allows.
	 *
	 * Not yet consulted by the module compiler, so it changes no behaviour alone
	 * (docs/backends/PRODUCTION_GAPS.md section 10).
	 */
	static bool signatureNeedsFrame(const WASM::FuncType& function);

	/*!
	 * \brief Reports whether any signature in a module needs the frame form.
	 * \param module Module whose function types are examined.
	 * \param optIn Whether the process-wide opt-in is set.
	 * \return True when the module must use the frame form throughout.
	 *
	 * Option 1 of docs/backends/PRODUCTION_GAPS.md section 11: the mode is module-wide,
	 * so one signature the register form cannot deliver decides for all of them.
	 */
	static bool moduleNeedsFrame(const WASM::Module& module, bool optIn);
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
