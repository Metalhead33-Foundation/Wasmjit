#ifndef LIBJITIMPORTRESOLVER_HPP
#define LIBJITIMPORTRESOLVER_HPP
#include "../WasmBase/WasmRegistryImportResolver.hpp"

/*!
  \file LibJitImportResolver.hpp
  \brief An alias of the backend-independent import resolver.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {

/*!
 * \brief Import resolver exposed in the LibJIT namespace.
 *
 * This is an alias, not a backend-specific implementation. Registered
 * function pointers must obey the LibJIT/WebAssembly callable ABI.
 */
using ImportResolver = WASM::RegistryImportResolver;

} // namespace LibJIT

#endif // LIBJITIMPORTRESOLVER_HPP
