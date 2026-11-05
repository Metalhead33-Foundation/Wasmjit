#ifndef WASMJIT_BINDINGS_FRONTEND_WIT_WITFRONTEND_HPP
#define WASMJIT_BINDINGS_FRONTEND_WIT_WITFRONTEND_HPP

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include "BindingModel.hpp"

/*!
  \file WitFrontend.hpp
  \brief WIT/WIT-JSON to the neutral binding schema.

  WIT is a build-time interface description. This front-end parses the JSON
  emitted by `wasm-tools component wit --json` and, for text WIT, asks the
  configured `wasm-tools` executable to produce that JSON first. It is tooling
  only: no WIT parser or package resolver is part of `WasmBase/`.
*/
namespace WASM::Bindings {

/*!
 * \brief Resolves one WIT type reference through a WIT JSON type table.
 *
 * Scalar aliases resolve to their underlying scalar; records, variants,
 * lists, resources and other structured types resolve to a named opaque WIT
 * reference.
 *
 * \param document A document containing a `types` array.
 * \param type A WIT type id, scalar spelling or type object.
 * \return The neutral schema value type.
 */
nlohmann::ordered_json witResolveType(const nlohmann::ordered_json& document,
		const nlohmann::ordered_json& type);

/*!
 * \brief Returns a deterministic target-language name for one WIT type id.
 *
 * A named WIT type keeps its WIT spelling. An anonymous structured type gets a
 * semantic name derived from its kind and children, with the type-table id
 * appended so that duplicate structural types remain distinct identifiers.
 *
 * \param document A document containing a `types` array.
 * \param id The WIT type-table id.
 * \return The unsanitized display name.
 */
std::string witTypeDisplayName(const nlohmann::ordered_json& document,
		std::size_t id);

/*!
 * \brief Selects and converts one WIT world from a WIT JSON document.
 *
 * The first world is used when \p worldName is empty. A world function becomes
 * a function entry; an interface import/export contributes its functions under
 * the interface name. The result is a complete schema, then projected to
 * \p direction exactly like a descriptor-derived schema.
 *
 * \param document The `wasm-tools component wit --json` document.
 * \param direction The requested output direction.
 * \param moduleName The module/component name in the schema envelope.
 * \param worldName The world to select, or empty for the first world.
 * \param error Receives a diagnostic when the document or world is invalid.
 * \return The projected typed schema, or an empty schema on error.
 */
Schema witSchema(const nlohmann::ordered_json& document, Direction direction,
		std::string_view moduleName, std::string_view worldName,
		std::string& error);

/*!
 * \brief Loads a WIT JSON file or a WIT text/package path.
 *
 * A `.json` path is parsed directly. Any other path is passed to
 * `wasm-tools component wit --json`; the `WASM_TOOLS` environment variable can
 * override the executable name. The text path may be a `.wit` file or a
 * WIT package directory.
 *
 * \param path WIT JSON, WIT text, or WIT package directory.
 * \param direction The requested output direction.
 * \param moduleName The module/component name in the schema envelope.
 * \param worldName The world to select, or empty for the first world.
 * \param forceJson Treat \p path as WIT JSON even without a `.json` extension.
 * \param error Receives a diagnostic when the file/tool/JSON is invalid.
 * \return The projected typed schema, or an empty schema on error.
 */
Schema witSchemaFromFile(const std::filesystem::path& path, Direction direction,
		std::string_view moduleName, std::string_view worldName, bool forceJson,
		std::string& error);

} // namespace WASM::Bindings

#endif // WASMJIT_BINDINGS_FRONTEND_WIT_WITFRONTEND_HPP
