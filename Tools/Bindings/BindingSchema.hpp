#ifndef WASMJIT_BINDINGS_BINDINGSCHEMA_HPP
#define WASMJIT_BINDINGS_BINDINGSCHEMA_HPP

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include "WasmBase/WasmComponent.hpp"
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

/*! \brief A target language the schema can be emitted for. */
enum class Language {
	CHeader, //!< A C/C++ header.
	Rust, //!< A Rust module.
	TypeScript, //!< TypeScript declarations.
	AssemblyScript, //!< AssemblyScript declarations.
	Zig, //!< Zig declarations.
	Kotlin, //!< Kotlin/Wasm declarations.
	MoonBit, //!< MoonBit declarations.
	Go, //!< Go Wasm declarations.
	Dart //!< Dart FFI declarations.
};

/*!
 * \brief Returns the CLI spelling of a target language.
 * \param language The language to name.
 * \return The lower-case language name.
 */
const char* languageName(Language language) noexcept;

/*!
 * \brief Parses a CLI language name.
 * \param name The name to parse.
 * \param language Receives the parsed language on success.
 * \return true when the name is a known language.
 */
bool parseLanguage(std::string_view name, Language& language) noexcept;

/*!
 * \brief Emits a schema in the requested target language.
 * \param schema The schema produced by `moduleSchema()`.
 * \param language The target language.
 * \return The generated source text.
 */
std::string emit(const nlohmann::ordered_json& schema, Language language);

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
 * \brief Projects a validated Component Model component into the binding schema.
 *
 * Component-level imports and exports are projected from the AST and the
 * canonical component type records. WIT JSON is not required; when a component
 * was built from WIT, its interface names remain the export names. Function
 * entries also carry an `abi` object — the exact flattened core parameter and
 * result cells plus the indirect flags from `TypeStore::canonicalAbi()`. The
 * function does not instantiate or execute the component.
 *
 * \param component The parsed and validated component.
 * \param types The store's type registry, used to resolve component type ids.
 * \param direction The adapter direction recorded in the envelope.
 * \param componentName The name recorded in the schema envelope.
 * \return The ordered JSON schema.
 */
nlohmann::ordered_json componentSchema(const WASM::Component& component,
		const WASM::TypeRegistry& types, Direction direction,
		std::string_view componentName = "component");

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
