#ifndef LIBJITIMPORTRESOLVER_HPP
#define LIBJITIMPORTRESOLVER_HPP
#include "../WasmBase/WasmRegistryImportResolver.hpp"

/*!
  \file LibJitImportResolver.hpp
  \brief An alias of the backend-independent import resolver.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {

//! Thin alias for callers that want to stay in the LibJIT namespace.
//! The registry itself is backend-independent; the LibJIT-specific part is
//! simply that registered imported function pointers must obey the LibJIT/Wasm
//! callable ABI.
using ImportResolver = WASM::RegistryImportResolver;

} // namespace LibJIT

#endif // LIBJITIMPORTRESOLVER_HPP
