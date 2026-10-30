#ifndef WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP
#define WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP

#include "../WasmBase/WasmType.hpp"
#include <asmjit/core.h>

/*!
  \file AsmjitTypeTranslator.hpp
  \brief Maps Wasm value and storage types to AsmJit types.

  This is an internal backend component, not an embedder-facing API.
*/

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
	 * \brief Builds the native function signature used by generated code.
	 * \param function Wasm function type.
	 * \return C-declaration host signature, including the implicit VMContext*.
	 * \throws std::runtime_error for unsupported types or too many arguments.
	 */
	static asmjit::FuncSignature translateFunctionType(const WASM::FuncType& function);
};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP
