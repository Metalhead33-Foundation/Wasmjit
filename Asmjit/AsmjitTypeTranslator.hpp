#ifndef WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP
#define WASMJIT_ASMJIT_TYPE_TRANSLATOR_HPP

#include "../WasmBase/WasmType.hpp"
#include <asmjit/core.h>

/*!
  \file AsmjitTypeTranslator.hpp
  \brief Maps the currently supported Wasm scalar ABI types to AsmJit types.

  This is an internal backend component, not an embedder-facing API.
*/

namespace AsmJitBackend {

/*!
 * \brief Builds AsmJit host-ABI signatures for the supported Wasm value subset.
 *
 * The engine's native function ABI prepends VMContext* to every function.
 * Multi-value results use a trailing void* result-buffer argument and a void
 * return, matching docs/proposals/MULTI_VALUE.md MV5. References are opaque
 * pointers at this ABI boundary.
 *
 * V128, packed storage parameters/results, and unsupported value encodings
 * are rejected explicitly rather than silently lowered incorrectly.
 */
class TypeTranslator
{
public:
	/*!
	 * \brief Maps one ordinary Wasm value type to an AsmJit scalar TypeId.
	 * \param type Wasm value type.
	 * \return Native AsmJit type identifier.
	 * \throws std::runtime_error when the type is not yet supported.
	 */
	static asmjit::TypeId translateValueType(const WASM::ValueType& type);

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
