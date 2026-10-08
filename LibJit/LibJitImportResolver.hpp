#ifndef LIBJITIMPORTRESOLVER_HPP
#define LIBJITIMPORTRESOLVER_HPP
#include "../WasmBase/WasmRegistryImportResolver.hpp"

namespace LibJIT {

// Thin alias for callers that want to stay in the LibJIT namespace.
// The registry itself is backend-independent; the LibJIT-specific part is
// simply that registered imported function pointers must obey the LibJIT/Wasm
// callable ABI.
using ImportResolver = WASM::RegistryImportResolver;

} // namespace LibJIT

#endif // LIBJITIMPORTRESOLVER_HPP
