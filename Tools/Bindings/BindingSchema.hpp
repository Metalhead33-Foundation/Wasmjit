#ifndef WASMJIT_BINDINGS_BINDINGSCHEMA_HPP
#define WASMJIT_BINDINGS_BINDINGSCHEMA_HPP

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmTypeRegistry.hpp"

/*!
  \file BindingSchema.hpp
  \brief The neutral binding schema and the first-party C/C++ and Rust emitters.

  This is tooling, not an engine header (docs/EMBEDDING_SURFACE.md §12.8). It
  consumes the public descriptor metadata on a parsed `WASM::Module`; it must
  never be included from `WasmBase/`, a backend or a plugin.
*/
namespace WASM::Bindings {

/*! \brief The direction a generated adapter faces. */
enum class Direction {
	GuestToHost, //!< A native host calls a module/component export.
	HostToGuest, //!< A Wasm guest imports a host function.
	GuestToGuest //!< One module/component calls another.
};

/*!
 * \brief Returns the lower-case schema spelling of a direction.
 * \param direction The direction to name.
 * \return `"g2h"`, `"h2g"` or `"g2g"`.
 */
const char* directionName(Direction direction) noexcept;

/*!
 * \brief Projects a parsed core module into the neutral binding schema.
 *
 * The schema is a projection of the engine's public metadata, not an IDL. It
 * never copies a numeric `TypeId` into generated code; a defined reference is
 * identified by its kind for emission and keeps its `TypeId` only as tooling
 * metadata.
 *
 * \param module The parsed core module.
 * \param types The store's type registry, used to resolve canonical types.
 * \param direction The adapter direction recorded in the envelope.
 * \param moduleName The name recorded in the schema envelope.
 * \return The ordered JSON schema.
 */
nlohmann::ordered_json moduleSchema(const WASM::Module& module,
		const WASM::TypeRegistry& types, Direction direction,
		std::string_view moduleName = "module");

/*!
 * \brief Emits a C header from a binding schema.
 *
 * Reference types become opaque `wasm_*` types; no layout, numeric `TypeId` or
 * `sizeof` contract is emitted. Reference parameters are marked borrowed and
 * reference results carry an explicit ownership note.
 *
 * \param schema The schema produced by `moduleSchema()`.
 * \return The C header text.
 */
std::string emitCHeader(const nlohmann::ordered_json& schema);

/*!
 * \brief Emits a Rust module from a binding schema.
 *
 * Exports become `unsafe extern "C"` function-pointer aliases; imports become
 * `extern "C"` declarations. Reference types are opaque zero-sized structs, and
 * reference parameters/results carry the same ownership comments as the C
 * header.
 *
 * \param schema The schema produced by `moduleSchema()`.
 * \return The Rust source text.
 */
std::string emitRustModule(const nlohmann::ordered_json& schema);

} // namespace WASM::Bindings

#endif // WASMJIT_BINDINGS_BINDINGSCHEMA_HPP
