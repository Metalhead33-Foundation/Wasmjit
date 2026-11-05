#ifndef WASMJIT_BINDINGS_BINDINGSCHEMA_HPP
#define WASMJIT_BINDINGS_BINDINGSCHEMA_HPP

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include "BindingModel.hpp"
#include "WasmBase/WasmComponent.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmTypeRegistry.hpp"

/*!
  \file BindingSchema.hpp
  \brief The public binding-schema API and descriptor projection.

  This is tooling, not an engine header (docs/EMBEDDING_SURFACE.md §12.8). It
  consumes the public descriptor metadata on a parsed `WASM::Module` or
  `WASM::Component`; the typed model lives in `BindingModel.hpp` and the
  language emitters live in the `Emit*.cpp` translation units. Never include
  this from `WasmBase/`, a backend or a plugin.
*/
namespace WASM::Bindings {

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
	Dart, //!< Dart FFI declarations.
	Java, //!< Java/TeaVM or JWebAssembly declarations.
	CSharp, //!< C#/.NET Wasm declarations.
	FSharp, //!< F#/.NET Wasm declarations.
	Python, //!< Python `ctypes` declarations.
	Ruby, //!< Ruby/Fiddle declarations.
	Lua, //!< LuaJIT FFI declarations.
	Swift, //!< Swift/C convention declarations.
	DLang, //!< D `extern(C)` declarations.
	Nim, //!< Nim `importc`/`exportc` declarations.
	Crystal, //!< Crystal `lib` bindings.
	Ada, //!< Ada `pragma Import`/`Export` bindings.
	Pascal, //!< Pascal `cdecl` external declarations.
	Haskell, //!< Haskell `foreign import ccall` declarations.
	CppAdapter, //!< A C++ adapter that dispatches through `WASM::callCallable`.
	CppTypedefs, //!< Host-side `Callable` function-pointer typedefs.
	CppSource //!< A C++ source with `Callable::fnPtr` cast helpers.
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
 * \brief Emits a typed schema in the requested target language.
 * \param schema The typed schema produced by `moduleSchemaModel()`.
 * \param language The target language.
 * \return The generated source text.
 */
std::string emit(const Schema& schema, Language language);

/*!
 * \brief Emits a JSON schema in the requested target language.
 * \param schema The schema produced by `moduleSchema()`.
 * \param language The target language.
 * \return The generated source text.
 */
std::string emit(const nlohmann::ordered_json& schema, Language language);

/*!
 * \brief Projects an imported complete schema into a requested direction.
 *
 * The input should be the `"all"` form produced by `--emit schema --direction all`
 * (or a hand-authored equivalent): it must retain both imports and exports.
 * A directional input can only be returned unchanged when the requested
 * direction matches it.
 *
 * \param schema The complete imported schema.
 * \param direction The requested output direction.
 * \return A projected schema with `direction` set accordingly.
 * \throws std::runtime_error when the input is malformed or cannot supply the
 * requested direction.
 */
nlohmann::ordered_json projectSchema(const nlohmann::ordered_json& schema,
		Direction direction);

/*!
 * \brief Projects a typed complete schema into a requested direction.
 * \param schema The complete typed schema.
 * \param direction The requested output direction.
 * \return A projected typed schema.
 * \throws std::runtime_error when the direction cannot be projected.
 */
Schema projectSchema(const Schema& schema, Direction direction);

/*!
 * \brief Projects a parsed core module into the typed binding schema.
 * \param module The parsed core module.
 * \param types The store's type registry.
 * \param direction The adapter direction recorded in the envelope.
 * \param moduleName The name recorded in the schema envelope.
 * \return The typed schema.
 */
Schema moduleSchemaModel(const WASM::Module& module,
		const WASM::TypeRegistry& types, Direction direction,
		std::string_view moduleName = "module");

/*!
 * \brief Projects a parsed core module into the neutral binding schema.
 *
 * The schema is a projection of the engine's public metadata, not an IDL. It
 * never copies a numeric `TypeId` into generated code; a defined reference is
 * identified by its kind for emission and keeps its `TypeId` only as tooling
 * metadata.
 *
 * For `GuestToGuest`, `imports` is projected from the module's **exports**,
 * using `moduleName` as the Wasm import module; the module's own imports are
 * not part of the consumer interface.
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
 * For `GuestToGuest`, `imports` is projected from the component's **exports**,
 * using `componentName` as the Wasm import module; the component's own imports
 * are not part of the consumer interface.
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
 * \brief Projects a validated component into the typed binding schema.
 * \param component The parsed and validated component.
 * \param types The store's type registry.
 * \param direction The adapter direction recorded in the envelope.
 * \param componentName The name recorded in the schema envelope.
 * \return The typed schema.
 */
Schema componentSchemaModel(const WASM::Component& component,
		const WASM::TypeRegistry& types, Direction direction,
		std::string_view componentName = "component");

/*!
 * \brief Emits a C header from a typed binding schema.
 * \param schema The typed schema.
 * \return The C header text.
 */
std::string emitCHeader(const Schema& schema);

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
 * \brief Emits a Rust module from a typed binding schema.
 * \param schema The typed schema.
 * \return The Rust source text.
 */
std::string emitRustModule(const Schema& schema);

/*!
 * \brief Emits a Rust module from a binding schema.
 *
 * Exports become `unsafe extern "C"` function-pointer aliases plus compilable
 * `#[export_name]` skeletons; imports become `#[link(wasm_import_module)]`
 * `extern "C"` blocks with `#[link_name]` declarations. Reference types are
 * opaque zero-sized structs, and reference parameters/results carry the same
 * ownership comments as the C header.
 *
 * \param schema The schema produced by `moduleSchema()`.
 * \return The Rust source text.
 */
std::string emitRustModule(const nlohmann::ordered_json& schema);

} // namespace WASM::Bindings

#endif // WASMJIT_BINDINGS_BINDINGSCHEMA_HPP